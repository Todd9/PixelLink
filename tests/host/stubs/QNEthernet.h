#pragma once
#include "Arduino.h"
#include <deque>
#include <vector>
#include <string>
#include <algorithm>
extern std::deque<std::vector<uint8_t>> rxq; extern std::vector<uint8_t> cur; extern std::vector<std::vector<uint8_t>> sent;
namespace qindesign { namespace network { namespace driver { inline void init(){} }
struct EthernetClass { bool begin(){return true;} bool begin(const IPAddress&,const IPAddress&,const IPAddress&){return true;}
 bool waitForLocalIP(uint32_t){return true;} void macAddress(uint8_t* m){memset(m,0xAB,6);} void setHostname(const char*){} void setDHCPEnabled(bool){} IPAddress localIP(){return IPAddress();} void end(){} };
extern EthernetClass Ethernet;
class EthernetUDP { public: explicit EthernetUDP(size_t){} uint8_t begin(uint16_t){return 1;}
 int parsePacket(){ if(rxq.empty()) return -1; cur=rxq.front(); rxq.pop_front(); return (int)cur.size(); }
 const uint8_t* data(){return cur.data();}
 IPAddress remoteIP(){return IPAddress();} uint16_t remotePort(){return 9999;}
 bool send(const IPAddress&,uint16_t,const uint8_t* d,size_t n){ sent.emplace_back(d,d+n); return true;} };
}}
// TCP stand-in for the firmware update server: one scripted client at a time.
// A test fills tcp.in, sets tcp.pending, and reads the node's replies from tcp.out.
struct FakeTcp { bool pending = false; bool open = false; std::string in; size_t pos = 0; std::string out; };
inline FakeTcp tcp;
namespace qindesign { namespace network {
class EthernetClient { public: EthernetClient() = default; explicit EthernetClient(bool v) : valid_(v) {}
  explicit operator bool() const { return valid_; }
  bool connected() { return valid_ && (tcp.open || tcp.pos < tcp.in.size()); }
  int read(uint8_t* b, size_t n) { if (!valid_ || tcp.pos >= tcp.in.size()) return -1;
    size_t k = std::min(n, tcp.in.size() - tcp.pos); memcpy(b, tcp.in.data() + tcp.pos, k); tcp.pos += k; return int(k); }
  size_t writeFully(const char* s) { if (!valid_) return 0; tcp.out += s; return strlen(s); }
  void flush() {} void stop() { if (valid_) tcp.open = false; valid_ = false; }
 private: bool valid_ = false; };
class EthernetServer { public: explicit EthernetServer(uint16_t) {} bool begin() { return true; }
  EthernetClient accept() { if (!tcp.pending) return EthernetClient(false); tcp.pending = false; tcp.open = true; return EthernetClient(true); } };
}}
