// Receiver test: runs the real PixelLinkReceiver sketch on the desktop with
// stand-ins for the Teensy (stubs/), a fake clock, a scripted UDP queue and a
// fake ObjectFLED that records every frame shown.
#include "Arduino.h"
#include <deque>
#include <vector>
#include <cassert>
#include <iostream>
std::deque<std::vector<uint8_t>> rxq; std::vector<uint8_t> cur; std::vector<std::vector<uint8_t>> sent;
#include "ObjectFLED.h"
static uint32_t fakeMs = 0;
uint32_t millis(){ return fakeMs; } uint32_t micros(){ return fakeMs*1000; }
void pinMode(uint8_t,uint8_t){} void digitalWriteFast(uint8_t,uint8_t){} float tempmonGetTemp(){return 42.5f;}
SerialT Serial;
struct Show { uint16_t leds; uint8_t pins; std::vector<uint8_t> data; };
std::vector<Show> shows; static uint16_t gLeds; static uint8_t gPins; static void* gBuf; int liveDrivers=0; int builds=0;
ObjectFLED::ObjectFLED(uint16_t n, void* b, uint8_t, uint8_t np, const uint8_t*, uint8_t){ gLeds=n; gPins=np; gBuf=b; liveDrivers++; builds++; assert(liveDrivers==1);}
ObjectFLED::~ObjectFLED(){ liveDrivers--; }
void ObjectFLED::begin(uint16_t){}
void ObjectFLED::show(){ Show s{gLeds,gPins,std::vector<uint8_t>((uint8_t*)gBuf,(uint8_t*)gBuf+gLeds*3)}; shows.push_back(s);}
int ObjectFLED::busy(){ return 0; }
#include "QNEthernet.h"
namespace qindesign{namespace network{ EthernetClass Ethernet; }}
#include "../../firmware/PixelLinkReceiver/PixelLinkReceiver.ino"

static void run(uint32_t ms){ for(uint32_t i=0;i<ms;i++){ fakeMs++; loop(); } }
static uint16_t F=0;
static void sendFrame(int nch, int len, uint8_t val, bool push=true, bool sync=false){
  F++;
  std::vector<uint8_t> rgb(len*3, val);
  for(int ch=0; ch<nch; ch++){
    for(int off=0; off<len; off+=486){
      int cnt = std::min(486, len-off);
      std::vector<uint8_t> b(12+cnt*3);
      bool last = (ch==nch-1) && (off+cnt>=len);
      pl::buildPixels(b.data(), F, ch, (push&&last)?pl::kFlagPush:0, off, cnt, rgb.data());
      rxq.push_back(b);
    }
  }
  if(sync){ std::vector<uint8_t> b(8); pl::buildSync(b.data(),F); rxq.push_back(b);}
}
#define CHECK(c) do{ if(!(c)){ std::cout<<"FAIL line "<<__LINE__<<": "#c<<"\n"; return 1;} }while(0)
int main(){
  setup();
  CHECK(shows.size()==1 && shows[0].pins==32 && shows[0].leds==32*486); // boot blank
  CHECK(mode==Mode::WaitForData);
  // IF_NO_SIGNAL: test starts after 5 s
  run(5100); CHECK(mode==Mode::Test);
  size_t s0=shows.size(); run(1000); CHECK(shows.size()>s0);
  CHECK(shows.back().data[0]==0 && shows.back().data[1]==64); // green phase (RGB stage, driver converts)
  // data interrupts test
  sendFrame(16, 100, 200); run(1);
  CHECK(mode==Mode::Receiving);
  CHECK(shows.back().pins==16 && shows.back().leds==16*100 && shows.back().data[0]==200);
  // implicit frames: no push
  size_t imp0=implicitFrames; sendFrame(16,100,150,false); run(1); sendFrame(16,100,151,false); run(1);
  CHECK(implicitFrames==imp0+1); CHECK(shows.back().data[0]==150);
  // SYNC path
  sendFrame(16,100,77,false,true); run(1); CHECK(shows.back().data[0]==77);
  // grow: 8 channels of 900 px (spans 2 packets) -> mask stays 16 ch for now, len grows to 900
  sendFrame(8, 900, 10); run(1);
  CHECK(shows.back().pins==16 && shows.back().leds==16*900);
  // keep sending small frames for >4 s -> shrink to 4 ch x 50
  for(int i=0;i<250;i++){ sendFrame(4,50,33); run(20);} 
  CHECK(shows.back().pins==4 && shows.back().leds==4*50 && shows.back().data[0]==33);
  // LED limit: 32 ch x 972 -> clamp to 682
  sendFrame(32, 972, 5); run(1);
  CHECK(shows.back().pins==32 && shows.back().leds==32*682);
  CHECK(limitClamps>0);
  // late packet from old frame ignored
  { std::vector<uint8_t> b(12+3); uint8_t px[3]={1,2,3}; pl::buildPixels(b.data(), F-3, 0, pl::kFlagPush, 0, 1, px); size_t n=shows.size(); rxq.push_back(b); run(1); CHECK(shows.size()==n);}  
  // sender restart (frame jumps far back) accepted
  F=0; sendFrame(4,50,99); run(1); CHECK(shows.back().data[0]==99);
  // bad packets counted
  uint32_t bad0=badPackets; rxq.push_back({1,2,3}); std::vector<uint8_t> b(12); pl::buildPixels(b.data(),F,40,0,0,0,nullptr); rxq.push_back(b); run(1); CHECK(badPackets==bad0+2);
  // discover -> info
  { std::vector<uint8_t> d(4); pl::buildDiscover(d.data()); rxq.push_back(d); run(1); pl::Info inf; CHECK(pl::parseInfo(sent.back().data(), sent.back().size(), inf)); CHECK(std::string(inf.name)=="remote" && inf.maxChannels==32 && inf.maxPixelsPerChannel==972);}  
  // link loss: hold 10 s, then fade 3 s, then dark
  run(9000); CHECK(mode==Mode::Receiving);
  run(1500); CHECK(mode==Mode::Fading);
  uint8_t mid = shows.back().data[0]; CHECK(mid<99 && mid>0);
  run(3000); CHECK(mode==Mode::Dark); CHECK(shows.back().data[0]==0);
  // data after dark resumes
  sendFrame(4,50,120); run(1); CHECK(mode==Mode::Receiving && shows.back().data[0]==120);
  std::cout<<"ALL PASS  shows="<<shows.size()<<" builds="<<builds<<" implicit="<<implicitFrames<<"\n";
}
void yield(){}
volatile uint16_t WDOG1_WCR = 0;
extern "C" { int firmware_buffer_init(uint32_t*,uint32_t*){return 0;} void firmware_buffer_free(uint32_t,uint32_t){}
 int flash_write_block(uint32_t,char*,uint32_t){return 0;} int check_flash_id(uint32_t,uint32_t){return 0;} void flash_move(uint32_t,uint32_t,uint32_t){} }
