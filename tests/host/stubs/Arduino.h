#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DMAMEM
#define OUTPUT 1
#define LOW 0
uint32_t millis(); uint32_t micros();
void pinMode(uint8_t,uint8_t); void digitalWriteFast(uint8_t,uint8_t);
float tempmonGetTemp(); void yield();
extern volatile uint16_t WDOG1_WCR;
struct SerialT { void begin(int){} explicit operator bool(){return true;} template<class...A> void printf(const char* f, A... a){ ::printf(f,a...);} };
extern SerialT Serial;
class IPAddress { public: uint8_t b[4]={0}; IPAddress(){} IPAddress(const uint8_t* p){memcpy(b,p,4);} uint8_t operator[](int i) const {return b[i];} };
