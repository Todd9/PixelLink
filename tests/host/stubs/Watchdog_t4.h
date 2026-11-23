#pragma once
struct WDT1 {}; struct WDT_timings_t { double timeout = 1; };
template<class T> class WDT_T4 { public: void begin(WDT_timings_t){} void feed(){} };
