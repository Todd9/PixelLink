// Firmware update test: drives the receiver's OtaUpdater (PixelLinkOta.h)
// through scripted TCP sessions against a fake flash, covering a good
// upload, each rejection, CANCEL and the 10 s timeout.

#include "Arduino.h"
#include "QNEthernet.h"
extern "C" {
#include "FlashTxx.h"
}
#include <string>
#include <vector>

// ---- Platform stand-ins --------------------------------------------------

static uint32_t fakeMs = 0;
uint32_t millis() { return fakeMs++; }  // advances, so the install delay ends
uint32_t micros() { return fakeMs * 1000; }
void yield() {}
SerialT Serial;
std::deque<std::vector<uint8_t>> rxq; std::vector<uint8_t> cur; std::vector<std::vector<uint8_t>> sent;
namespace qindesign { namespace network { EthernetClass Ethernet; } }

// Fake flash: a 1 MB buffer region at 0x60400000. Writes must be sequential,
// as with FlasherX.
static const uint32_t kBuf = 0x60400000, kSize = 0x100000;
static std::vector<uint8_t> flash(kSize, 0xFF);
static uint32_t nextAddr = 0;
static int bufFrees = 0;
static bool moved = false;
static uint32_t movedSize = 0;
static std::vector<uint8_t> installed;

extern "C" int firmware_buffer_init(uint32_t* a, uint32_t* s) { *a = kBuf; *s = kSize; nextAddr = kBuf; return FLASH_BUFFER_TYPE; }
extern "C" void firmware_buffer_free(uint32_t, uint32_t) { std::fill(flash.begin(), flash.end(), 0xFF); bufFrees++; }
extern "C" int flash_write_block(uint32_t addr, char* d, uint32_t n) {
  if (addr < kBuf || addr + n > kBuf + kSize || addr < nextAddr) return 2;
  memcpy(&flash[addr - kBuf], d, n);
  nextAddr = addr + n;
  return 0;
}
extern "C" int check_flash_id(uint32_t b, uint32_t s) {
  const char* id = FLASH_ID;
  const size_t L = strlen(id);
  for (uint32_t i = 0; i + L <= s; i++) if (!memcmp(&flash[b - kBuf + i], id, L)) return 1;
  return 0;
}
extern "C" void flash_move(uint32_t dst, uint32_t src, uint32_t size) {
  moved = (dst == FLASH_BASE_ADDR);
  movedSize = size;
  installed.assign(flash.begin() + (src - kBuf), flash.begin() + (src - kBuf) + size);
}

#include "../../firmware/PixelLinkReceiver/PixelLinkOta.h"

// ---- Intel HEX -----------------------------------------------------------

static std::string record(uint16_t addr, uint8_t type, const uint8_t* data, uint8_t n) {
  char out[600];
  uint8_t sum = uint8_t(n + (addr >> 8) + (addr & 0xFF) + type);
  int len = snprintf(out, sizeof(out), ":%02X%04X%02X", n, addr, type);
  for (int i = 0; i < n; i++) { len += snprintf(out + len, 8, "%02X", data[i]); sum += data[i]; }
  snprintf(out + len, 8, "%02X\n", uint8_t(-sum));
  return out;
}

static std::vector<std::string> toHex(const std::vector<uint8_t>& blob, uint32_t base = FLASH_BASE_ADDR) {
  std::vector<std::string> lines;
  uint32_t upper = 0xFFFFFFFF;
  for (size_t off = 0; off < blob.size(); off += 16) {
    const uint32_t a = base + uint32_t(off);
    if ((a >> 16) != upper) {
      upper = a >> 16;
      const uint8_t u[2] = {uint8_t(upper >> 8), uint8_t(upper)};
      lines.push_back(record(0, 4, u, 2));
    }
    const uint8_t n = uint8_t(std::min<size_t>(16, blob.size() - off));
    lines.push_back(record(uint16_t(a), 0, &blob[off], n));
  }
  lines.push_back(":00000001FF\n");
  return lines;
}

static std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (auto& l : v) s += l;
  return s;
}

// ---- Harness -------------------------------------------------------------

static int feeds = 0, starts = 0, flashes = 0, ends = 0;
static void onFeed() { feeds++; }
static void onStart() { starts++; }
static void onFlash() { flashes++; }
static void onEnd() { ends++; }

static void resetAll() {
  tcp = FakeTcp();
  moved = false; movedSize = 0; installed.clear();
  starts = flashes = ends = 0;
}

static void pollFor(pixellink::OtaUpdater& ota, int n) { for (int i = 0; i < n; i++) ota.poll(); }

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main() {
  std::vector<uint8_t> blob(150000);
  uint32_t seed = 1;
  for (auto& b : blob) { seed = seed * 1103515245u + 12345u; b = uint8_t(seed >> 16); }
  memcpy(&blob[90000], FLASH_ID, strlen(FLASH_ID));
  const std::vector<std::string> good = toHex(blob);

  // 1. Good image: READY with matching counts, then GO installs it.
  {
    resetAll();
    pixellink::OtaUpdater ota(5151);
    ota.begin(onFeed, onStart, onFlash, onEnd);
    tcp.in = join(good);
    tcp.pending = true;
    pollFor(ota, 2000);
    char expect[64];
    snprintf(expect, sizeof(expect), "READY %zu %zu\n", good.size(), blob.size());
    CHECK(tcp.out == expect);
    CHECK(ota.active() && starts == 1 && !moved);
    tcp.in += "GO\n";
    pollFor(ota, 10);
    CHECK(tcp.out == std::string(expect) + "FLASHING\n");
    CHECK(flashes == 1 && moved && movedSize == blob.size() && installed == blob);
  }

  // 2. No FLASH_ID: rejected, buffer freed, running firmware untouched.
  {
    resetAll();
    std::vector<uint8_t> noid = blob;
    memset(&noid[90000], 'x', strlen(FLASH_ID));
    pixellink::OtaUpdater ota(5151);
    ota.begin(onFeed, onStart, onFlash, onEnd);
    const int frees = bufFrees;
    tcp.in = join(toHex(noid));
    tcp.pending = true;
    pollFor(ota, 2000);
    CHECK(tcp.out == "ERROR not OTA-capable firmware (no FLASH_ID)\n");
    CHECK(!ota.active() && ends == 1 && bufFrees == frees + 1 && !moved);
  }

  // 3. A record with a bad checksum.
  {
    resetAll();
    std::vector<std::string> bad = good;
    std::string& l = bad[500];
    l[l.size() - 2] = (l[l.size() - 2] == '0') ? '1' : '0';
    pixellink::OtaUpdater ota(5151);
    ota.begin(onFeed, onStart, onFlash, onEnd);
    tcp.in = join(bad);
    tcp.pending = true;
    pollFor(ota, 2000);
    CHECK(tcp.out == "ERROR bad hex record\n");
    CHECK(!ota.active() && !moved);
  }

  // 4. An image that doesn't start at the flash base.
  {
    resetAll();
    pixellink::OtaUpdater ota(5151);
    ota.begin(onFeed, onStart, onFlash, onEnd);
    tcp.in = join(toHex(blob, FLASH_BASE_ADDR + 0x1000));
    tcp.pending = true;
    pollFor(ota, 2000);
    CHECK(tcp.out == "ERROR image does not start at flash base\n");
    CHECK(!ota.active() && !moved);
  }

  // 5. CANCEL after READY.
  {
    resetAll();
    pixellink::OtaUpdater ota(5151);
    ota.begin(onFeed, onStart, onFlash, onEnd);
    tcp.in = join(good) + "CANCEL\n";
    tcp.pending = true;
    pollFor(ota, 2000);
    CHECK(tcp.out.find("ERROR cancelled\n") != std::string::npos);
    CHECK(!ota.active() && ends == 1 && !moved);
    CHECK(std::string(ota.lastError()) == "cancelled");
  }

  // 6. The client goes quiet partway: cancelled after 10 s.
  {
    resetAll();
    pixellink::OtaUpdater ota(5151);
    ota.begin(onFeed, onStart, onFlash, onEnd);
    tcp.in = join(std::vector<std::string>(good.begin(), good.begin() + 100));
    tcp.pending = true;
    pollFor(ota, 200);
    CHECK(ota.active() && tcp.out.empty());
    fakeMs += 10001;
    pollFor(ota, 2);
    CHECK(tcp.out == "ERROR timeout\n" && !ota.active() && !moved);
  }

  CHECK(feeds > 0);
  if (failures) { printf("%d failure(s)\n", failures); return 1; }
  printf("ota_test: ALL PASS\n");
  return 0;
}
