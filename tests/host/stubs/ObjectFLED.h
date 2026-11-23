#pragma once
#include "Arduino.h"
#define CORDER_RGB 0
#define CORDER_GRB 2
class ObjectFLED { public:
 ObjectFLED(uint32_t numLEDs, void* drawBuf, uint8_t config, uint8_t numPins, const uint8_t* pinList, uint8_t serpentine = 0);
 ~ObjectFLED();
 void begin(void); void begin(uint16_t); void begin(double, uint16_t = 300); void begin(uint16_t, uint16_t, uint16_t, uint16_t = 300);
 void show(void); int busy(void); };
