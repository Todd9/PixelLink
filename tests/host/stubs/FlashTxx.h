#pragma once
#include <stdint.h>
#define FLASH_ID "fw_teensy41"
#define FLASH_BASE_ADDR 0x60000000u
#define NO_BUFFER_TYPE 0
#define RAM_BUFFER_TYPE 1
#define FLASH_BUFFER_TYPE 2
#define IN_FLASH(a) ((a) >= 0x60000000u && (a) < 0x60800000u)
int firmware_buffer_init(uint32_t* addr, uint32_t* size);
void firmware_buffer_free(uint32_t addr, uint32_t size);
int flash_write_block(uint32_t addr, char* data, uint32_t count);
int check_flash_id(uint32_t buffer, uint32_t size);
void flash_move(uint32_t dst, uint32_t src, uint32_t size);
