#pragma once
#include <stdint.h>
#include <stddef.h>
static inline uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t* p,size_t n) {
 crc=~crc; for(size_t i=0;i<n;i++){crc^=p[i];for(int k=0;k<8;k++)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));}return ~crc;
}
