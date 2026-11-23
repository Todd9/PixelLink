# Desktop tests for the Pixel Link library and receiver.
# Needs only g++ (C++17). Run from this folder:  make test

CXX      ?= g++
CXXFLAGS ?= -std=gnu++17 -O1 -Wall -Wno-unused-function -Wno-unused-variable
LIB      := ../../library/PixelLink/src
BUILD    := build

TESTS := protocol_test receiver_test ota_test

.PHONY: test clean
test: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $^; do ./$$t || exit 1; done

$(BUILD)/protocol_test: protocol_test.cpp $(LIB)/PixelLink.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(LIB) $< -o $@

$(BUILD)/receiver_test: receiver_test.cpp ../../firmware/PixelLinkReceiver/*.ino ../../firmware/PixelLinkReceiver/*.h stubs/*.h $(LIB)/PixelLink.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -Istubs -I$(LIB) $< -o $@

$(BUILD)/ota_test: ota_test.cpp ../../firmware/PixelLinkReceiver/PixelLinkOta.h stubs/*.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -Istubs -I$(LIB) $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
