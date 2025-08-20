#ifndef CA_LUT_H
#define CA_LUT_H

#include <Arduino.h>

// Fast branchless 16-bit abs
inline int16_t lutAbs16(int16_t v){ int16_t m = v >> 15; return (v ^ m) - m; }

// Fast clamp for 16-bit ints
inline int16_t lutClamp16(int16_t v, int16_t lo, int16_t hi){ return v<lo? lo : (v>hi? hi : v); }

// Q15 reciprocal LUT for 1..255 (recip[n] ~= round((1.0/n)*32768))
static const uint16_t LUT_RECIP_Q15[256] PROGMEM = {
  0, 32768, 16384, 10923, 8192, 6554, 5461, 4681, 4096, 3641, 3277, 2979, 2731, 2521, 2341, 2185,
  2048, 1928, 1820, 1723, 1638, 1561, 1491, 1429, 1374, 1323, 1280, 1240, 1205, 1172, 1144, 1118,
  1093, 1065, 1037, 1009, 983, 963, 936, 913,
  // NOTE: For AVR flash size, keep it short; we only need small n divisors commonly used
};

// Multiply by reciprocal: approx x / n for n in [1..255]
inline uint16_t lutDivU16byU8(uint16_t x, uint8_t n){
  if (n == 0) return 0; uint16_t rq15 = pgm_read_word(&LUT_RECIP_Q15[n]);
  return (uint16_t)(((uint32_t)x * rq15) >> 15);
}

#endif
