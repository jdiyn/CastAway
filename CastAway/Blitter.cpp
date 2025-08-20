#include "Blitter.h"
#include "LUT.h"

namespace { // just global
  // Single scanline buffer used by all compositors
  // s_back is the current line the renderer composes into
  static uint16_t s_lineA[320];
  static uint16_t* s_back = s_lineA;
  
  // Read helpers from PROGMEM. Using inline keeps call overhead low on AVR
  inline uint8_t  rd8(const uint8_t* p){ return pgm_read_byte(p); }
  inline uint16_t rd16(const uint16_t* p){ return pgm_read_word(p); }
}

// Raw LCD bus path removed.

#if !CA_AVR_FAST_BLIT
// Compose a 4bpp sprite over the current scanline without a color key
// Non-zero nibbles are drawn (index 0 is treated as background)
void CA_Blit::composeOver4bpp_P(const uint8_t* data, uint16_t w, uint16_t h,
                                int16_t vx, int16_t vy, const uint16_t* pal565,
                                bool hFlip, int16_t y, int16_t x0, int16_t wRegion){
  const int16_t row = y - vy; if (row < 0 || row >= (int16_t)h) return;
  const uint16_t bpr = (w + 1) >> 1;
  const uint8_t* src = data + (uint32_t)row * bpr;

  const int16_t dxBaseN = (int16_t)(vx - x0);
  const int16_t dxBaseF = (int16_t)((vx + (int16_t)w - 1) - x0);

  int16_t sxStart, sxEnd;
  if (!hFlip) {
    int16_t lo = -dxBaseN;
    int16_t hi = (wRegion - 1) - dxBaseN;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  } else {
    // FIX: use wRegion, not w
    int16_t lo = dxBaseF - ((int16_t)wRegion - 1);
    int16_t hi = dxBaseF;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  }

  // Iterate over packed bytes; each byte encodes two pixels (hi and lo nibble)
  int16_t jStart = sxStart >> 1;
  int16_t jEnd   = sxEnd   >> 1;
  for (int16_t j = jStart; j <= jEnd; ++j) {
    const uint8_t b = rd8(src + j);
    const int16_t sx0 = (j << 1);
    const int16_t sx1 = sx0 + 1;

    if (sx0 >= sxStart) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx0) : (int16_t)(dxBaseN + sx0);
      if ((uint16_t)dx < (uint16_t)wRegion) {
        const uint8_t ni = (b >> 4);
        if (ni) s_back[dx] = rd16(pal565 + ni);
      }
    }
    if (sx1 <= sxEnd) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx1) : (int16_t)(dxBaseN + sx1);
      if ((uint16_t)dx < (uint16_t)wRegion) {
        const uint8_t ni = (b & 0x0F);
        if (ni) s_back[dx] = rd16(pal565 + ni);
      }
    }
  }
}

// Compose a 4bpp sprite with a color key in RGB565 (skip if equals key565)
void CA_Blit::composeOver4bppKey_P(const uint8_t* data, uint16_t w, uint16_t h,
                                   int16_t vx, int16_t vy, uint16_t* paletteRam,
                                   bool hFlip, uint16_t key565,
                                   int16_t y, int16_t x0, int16_t wRegion){
  const int16_t row = y - vy; if (row < 0 || row >= (int16_t)h) return;
  const uint16_t bpr = (w + 1) >> 1;
  const uint8_t* src = data + (uint32_t)row * bpr;

  const int16_t dxBaseN = (int16_t)(vx - x0);
  const int16_t dxBaseF = (int16_t)((vx + (int16_t)w - 1) - x0);

  int16_t sxStart, sxEnd;
  if (!hFlip) {
    int16_t lo = -dxBaseN;
    int16_t hi = (wRegion - 1) - dxBaseN;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  } else {
    // FIX: use wRegion, not w
    int16_t lo = dxBaseF - ((int16_t)wRegion - 1);
    int16_t hi = dxBaseF;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  }

  int16_t jStart = sxStart >> 1;
  int16_t jEnd   = sxEnd   >> 1;
  for (int16_t j = jStart; j <= jEnd; ++j) {
    const uint8_t b = pgm_read_byte(src + j);
    const int16_t sx0 = (j << 1);
    const int16_t sx1 = sx0 + 1;

    if (sx0 >= sxStart) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx0) : (int16_t)(dxBaseN + sx0);
      if ((uint16_t)dx < (uint16_t)wRegion) {
        const uint8_t ni = (b >> 4);
        const uint16_t c = paletteRam[ni];
        if (c != key565) s_back[dx] = c;
      }
    }
    if (sx1 <= sxEnd) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx1) : (int16_t)(dxBaseN + sx1);
      if ((uint16_t)dx < (uint16_t)wRegion) {
        const uint8_t ni = (b & 0x0F);
        const uint16_t c = paletteRam[ni];
        if (c != key565) s_back[dx] = c;
      }
    }
  }
}

// Compose a 4bpp sprite with a transparent palette index (0..15) as the key
void CA_Blit::composeOver4bppKeyIdx_P(const uint8_t* data, uint16_t w, uint16_t h,
                                      int16_t vx, int16_t vy, uint16_t* paletteRam,
                                      bool hFlip, uint8_t keyIndex,
                                      int16_t y, int16_t x0, int16_t wRegion){
  const int16_t row = y - vy; if (row < 0 || row >= (int16_t)h) return;
  const uint16_t bpr = (w + 1) >> 1;
  const uint8_t* src = data + (uint32_t)row * bpr;

  const int16_t dxBaseN = (int16_t)(vx - x0);
  const int16_t dxBaseF = (int16_t)((vx + (int16_t)w - 1) - x0);

  int16_t sxStart, sxEnd;
  if (!hFlip) {
    int16_t lo = -dxBaseN;
    int16_t hi = (wRegion - 1) - dxBaseN;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  } else {
    int16_t lo = dxBaseF - ((int16_t)wRegion - 1);
    int16_t hi = dxBaseF;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  }

  int16_t jStart = sxStart >> 1;
  int16_t jEnd   = sxEnd   >> 1;
  for (int16_t j = jStart; j <= jEnd; ++j) {
    const uint8_t b = pgm_read_byte(src + j);
    const int16_t sx0 = (j << 1);
    const int16_t sx1 = sx0 + 1;

    if (sx0 >= sxStart) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx0) : (int16_t)(dxBaseN + sx0);
      if ((uint16_t)dx < (uint16_t)wRegion) {
        const uint8_t ni = (b >> 4);
        if (ni != keyIndex) s_back[dx] = paletteRam[ni];
      }
    }
    if (sx1 <= sxEnd) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx1) : (int16_t)(dxBaseN + sx1);
      if ((uint16_t)dx < (uint16_t)wRegion) {
        const uint8_t ni = (b & 0x0F);
        if (ni != keyIndex) s_back[dx] = paletteRam[ni];
      }
    }
  }
}
#endif // !CA_AVR_FAST_BLIT

// Fill a solid rectangle segment that intersects the current scanline.
void CA_Blit::composeSolidRectLine(int16_t y, int16_t x0, int16_t wRegion,
                                   int16_t rx, int16_t ry, int16_t rw, int16_t rh,
                                   uint16_t color565) {
  if (y < ry || y >= ry + rh) return;
  const int16_t x1 = x0 + wRegion;
  int16_t L = rx; if (L < x0) L = x0;
  int16_t R = rx + rw; if (R > x1) R = x1;
  
  const int16_t fillLen = R - L;
  if (fillLen <= 0) return;
  
  uint16_t* dst = s_back + (L - x0);
  
  // AVR-optimized solid fill (same as bar fill)
  #if defined(__AVR__) && CA_AVR_BG_FAST
  int16_t count = fillLen;
  asm volatile(
    "1:\n\t"
    "st Z+, %A2\n\t"     // Store low byte
    "st Z+, %B2\n\t"     // Store high byte  
    "sbiw %A1, 1\n\t"    // Decrement count
    "brne 1b\n\t"        // Branch if not zero
    : "+z" (dst), "+w" (count)
    : "r" (color565)
    : "memory"
  );
  #else
  for (int16_t i = 0; i < fillLen; ++i) {
    dst[i] = color565;
  }
  #endif
}


// Draw a 1px outline for a rectangle that intersects this scanline
void CA_Blit::composeRectOutlineLine(int16_t y, int16_t x0, int16_t wRegion,
                                     int16_t rx, int16_t ry, int16_t rw, int16_t rh,
                                     uint16_t color565) {
  const int16_t x1 = x0 + wRegion;
  const int16_t r  = rx + rw - 1;
  if (y == ry || y == (ry + rh - 1)) {
    int16_t L = rx; if (L < x0) L = x0;
    int16_t R = r + 1; if (R > x1) R = x1;
    for (int16_t x=L; x<R; ++x) s_back[x - x0] = color565;
    return;
  }
  if (y > ry && y < ry + rh - 1) {
    if (rx >= x0 && rx < x1) s_back[rx - x0] = color565;
    if (r  >= x0 && r  < x1) s_back[r  - x0] = color565;
  }
}

// Draw the filled portion of a horizontal bar within this scanline
void CA_Blit::composeHBarLine(int16_t y, int16_t x0, int16_t wRegion,
                              int16_t bx, int16_t by, int16_t bw, int16_t bh,
                              int16_t fillW, uint16_t color565) {
  if (y < by || y >= by + bh) return;
  if (fillW < 0) fillW = 0; if (fillW > bw) fillW = bw;
  const int16_t x1 = x0 + wRegion;
  int16_t L = bx; if (L < x0) L = x0;
  int16_t R = bx + fillW; if (R > x1) R = x1;
  
  const int16_t fillLen = R - L;
  if (fillLen <= 0) return;
  
  uint16_t* dst = s_back + (L - x0);
  
  // AVR-optimized bar fill for better tension bar performance
  #if defined(__AVR__) && CA_AVR_BG_FAST
  // Use fast assembly loop for bar fills
  int16_t count = fillLen;
  asm volatile(
    "1:\n\t"
    "st Z+, %A2\n\t"     // Store low byte
    "st Z+, %B2\n\t"     // Store high byte  
    "sbiw %A1, 1\n\t"    // Decrement count
    "brne 1b\n\t"        // Branch if not zero
    : "+z" (dst), "+w" (count)
    : "r" (color565)
    : "memory"
  );
  #else
  // Standard C loop fallback
  for (int16_t i = 0; i < fillLen; ++i) {
    dst[i] = color565;
  }
  #endif
}

// Compose a 320px-wide background scanline from a 160x120 indexed image split into 4 quads
// Each source pixel is replicated horizontally by 2 to achieve 2x scale
void CA_Blit::composeBGLine_160to320_quads_P(const uint8_t* q0, const uint8_t* q1,
                                             const uint8_t* q2, const uint8_t* q3,
                                             uint16_t w160, uint16_t h120,
                                             uint16_t cw, uint16_t ch,
                                             const uint16_t* pal565,
                                             int16_t y, int16_t x0, int16_t w){
  if (w <= 0) return;

  int16_t srcY = y >> 1;
  if (srcY < 0) srcY = 0; else if (srcY >= (int16_t)h120) srcY = (int16_t)h120 - 1;

  // Branchless quadrant selection for row base
  const uint8_t* baseQ[4] = { q0, q1, q2, q3 };
  const uint16_t leftW  = cw;
  const uint16_t rightW = (uint16_t)(w160 - cw);
  const uint8_t topMask = (uint8_t)(srcY >= (int16_t)ch);   // 0 for top, 1 for bottom
  const uint8_t baseIdx = (uint8_t)(topMask << 1);          // 0: {q0,q1}, 2: {q2,q3}
  const int16_t ly = (int16_t)(srcY - (topMask ? (int16_t)ch : 0));

  const uint8_t* rowL = baseQ[baseIdx + 0] + (uint32_t)ly * leftW;
  const uint8_t* rowR = baseQ[baseIdx + 1] + (uint32_t)ly * rightW;

  const int16_t splitX = (int16_t)cw << 1;               // screen x where right quad starts
  const int16_t xStart = x0;
  const int16_t xEnd   = (int16_t)(x0 + w);

  // Helper lambda: draw a segment (replicate each source pixel twice)
  auto drawSegment = [&](int16_t segL, int16_t segR, const uint8_t* row, uint16_t rowW, int16_t srcX0){
    if (segR <= segL) return;
  uint16_t* dst = s_back + (segL - x0);
  int16_t x = segL;
  const uint16_t* pal = pal565;

    // If starting on an odd screen x, emit the second half of the current source pixel
    int16_t sxi = (int16_t)(srcX0 + (segL >> 1));
    if (x & 1) {
      const uint16_t c = pal[ rd8(row + sxi) ];
      *dst++ = c;
      ++x;
      ++sxi; // consumed the second half -> advance to next source pixel
    }

    // Pairs, unrolled by 2 (write 4 pixels per iter). Optionally by 4 when CA_AVR_AGGR
    int16_t pairs = (int16_t)((segR - x) >> 1);
#if CA_AVR_AGGR
    // Aggressive 8-pixel unroll for maximum throughput
    while (pairs >= 8) {
      #if defined(__AVR__) && CA_AVR_BG_FAST
      const uint16_t c0 = pal[ rd8(row + sxi    ) ];
      const uint16_t c1 = pal[ rd8(row + sxi + 1) ];
      const uint16_t c2 = pal[ rd8(row + sxi + 2) ];
      const uint16_t c3 = pal[ rd8(row + sxi + 3) ];
      const uint16_t c4 = pal[ rd8(row + sxi + 4) ];
      const uint16_t c5 = pal[ rd8(row + sxi + 5) ];
      const uint16_t c6 = pal[ rd8(row + sxi + 6) ];
      const uint16_t c7 = pal[ rd8(row + sxi + 7) ];
      // Ultra-fast 16-store sequence
      asm volatile(
        "st Z+, %A1\n\t" "st Z+, %B1\n\t" "st Z+, %A1\n\t" "st Z+, %B1\n\t"
        "st Z+, %A2\n\t" "st Z+, %B2\n\t" "st Z+, %A2\n\t" "st Z+, %B2\n\t"
        "st Z+, %A3\n\t" "st Z+, %B3\n\t" "st Z+, %A3\n\t" "st Z+, %B3\n\t"
        "st Z+, %A4\n\t" "st Z+, %B4\n\t" "st Z+, %A4\n\t" "st Z+, %B4\n\t"
        "st Z+, %A5\n\t" "st Z+, %B5\n\t" "st Z+, %A5\n\t" "st Z+, %B5\n\t"
        "st Z+, %A6\n\t" "st Z+, %B6\n\t" "st Z+, %A6\n\t" "st Z+, %B6\n\t"
        "st Z+, %A7\n\t" "st Z+, %B7\n\t" "st Z+, %A7\n\t" "st Z+, %B7\n\t"
        "st Z+, %A8\n\t" "st Z+, %B8\n\t" "st Z+, %A8\n\t" "st Z+, %B8\n\t"
        : "+z" (dst)
        : "r" (c0), "r" (c1), "r" (c2), "r" (c3), "r" (c4), "r" (c5), "r" (c6), "r" (c7)
        : "memory"
      );
      sxi += 8; x += 16; pairs -= 8;
      #else
      for(int i = 0; i < 8; i++) {
        const uint16_t c = pal[ rd8(row + sxi + i) ];
        dst[i*2] = c; dst[i*2+1] = c;
      }
      dst += 16; sxi += 8; x += 16; pairs -= 8;
      #endif
    }
    while (pairs >= 4) {
      const uint16_t c0 = pal[ rd8(row + sxi    ) ];
      const uint16_t c1 = pal[ rd8(row + sxi + 1) ];
      const uint16_t c2 = pal[ rd8(row + sxi + 2) ];
      const uint16_t c3 = pal[ rd8(row + sxi + 3) ];
      // Fast duplicate stores
      #if defined(__AVR__) && CA_AVR_BG_FAST
      asm volatile(
        "st  Z+, %A1\n\t"  "st  Z+, %B1\n\t"
        "st  Z+, %A1\n\t"  "st  Z+, %B1\n\t"
        "st  Z+, %A2\n\t"  "st  Z+, %B2\n\t"
        "st  Z+, %A2\n\t"  "st  Z+, %B2\n\t"
        "st  Z+, %A3\n\t"  "st  Z+, %B3\n\t"
        "st  Z+, %A3\n\t"  "st  Z+, %B3\n\t"
        "st  Z+, %A4\n\t"  "st  Z+, %B4\n\t"
        "st  Z+, %A4\n\t"  "st  Z+, %B4\n\t"
        : "+z" (dst)
        : "r" (c0), "r" (c1), "r" (c2), "r" (c3)
        : "memory"
      );
      #else
      dst[0] = c0; dst[1] = c0; dst[2] = c1; dst[3] = c1;
      dst[4] = c2; dst[5] = c2; dst[6] = c3; dst[7] = c3;
      dst += 8;
      #endif
      sxi += 4; x += 8; pairs -= 4;
    }
#endif
    while (pairs >= 2) {
      const uint16_t c0 = pal[ rd8(row + sxi    ) ];
      const uint16_t c1 = pal[ rd8(row + sxi + 1) ];
      #if defined(__AVR__) && CA_AVR_BG_FAST
      asm volatile(
        "st  Z+, %A1\n\t"  "st  Z+, %B1\n\t"
        "st  Z+, %A1\n\t"  "st  Z+, %B1\n\t"
        "st  Z+, %A2\n\t"  "st  Z+, %B2\n\t"
        "st  Z+, %A2\n\t"  "st  Z+, %B2\n\t"
        : "+z" (dst)
        : "r" (c0), "r" (c1)
        : "memory"
      );
      #else
      dst[0] = c0; dst[1] = c0; dst[2] = c1; dst[3] = c1;
      dst += 4;
      #endif
      sxi += 2; x += 4; pairs -= 2;
    }
    if (pairs == 1) {
      const uint16_t c = pal[ rd8(row + sxi++) ];
      #if defined(__AVR__) && CA_AVR_BG_FAST
      asm volatile(
        "st  Z+, %A1\n\t"  "st  Z+, %B1\n\t"
        "st  Z+, %A1\n\t"  "st  Z+, %B1\n\t"
        : "+z" (dst)
        : "r" (c)
        : "memory"
      );
      #else
      dst[0] = c; dst[1] = c; dst += 2;
      #endif
      x += 2;
      pairs = 0;
    }

    // Trailing single
    if (x < segR) {
      *dst = pal[ rd8(row + sxi) ];
    }
  };

  // Left segment: [max(x0, 0) .. min(x0+w, splitX))
  int16_t L0 = xStart;
  int16_t R0 = xEnd;
  if (L0 < 0) L0 = 0;
  if (R0 > splitX) R0 = splitX;
  if (R0 > L0) drawSegment(L0, R0, rowL, leftW, 0);

  // Right segment: [max(x0, splitX) .. min(x0+w, 2*w160))
  int16_t L1 = xStart; if (L1 < splitX) L1 = splitX;
  int16_t R1 = xEnd;   const int16_t screenMax = (int16_t)w160 << 1;
  if (R1 > screenMax) R1 = screenMax;
  if (R1 > L1) {
    // Right quad srcX0 is offset by cw (we already clipped to right side)
    drawSegment(L1, R1, rowR, rightW, - (splitX >> 1));
  }
}

void CA_Blit::pushLinePhysicalNoAddr(const CA_BlitConfig& cfg, int16_t w, bool first){
  if (w <= 0) return;
  cfg.tft->pushColors(s_back, w, first);
}

// (Immediate 8bpp quad blitter removed; background uses scanline composer.)

// Expose the current line buffer pointer
uint16_t* CA_Blit::lineBuffer() {
  return s_back;
}

// Optional helper: poke a pixel into the current line buffer (no bounds checks)
void CA_Blit::pokeLinePixel(int16_t dx, uint16_t color565) {
  s_back[dx] = color565;
}

#if CA_AVR_FAST_BLIT
// AVR-optimized inner loops for 4bpp sprite composition
// These routines reduce branches and exploit packed nibbles for speed
// They avoid modulo operations and keep register pressure predictable
void CA_Blit::composeOver4bpp_AVR_P(const uint8_t* data, uint16_t w, uint16_t h,
                                    int16_t vx, int16_t vy, const uint16_t* pal565,
                                    bool hFlip, int16_t y, int16_t x0, int16_t wRegion){
  const int16_t row = (int16_t)(y - vy); if ((uint16_t)row >= h) return;
  const uint16_t bpr = (w + 1) >> 1;
  const uint8_t* src = data + (uint32_t)row * bpr;
  const int16_t dxBaseN = (int16_t)(vx - x0);
  const int16_t dxBaseF = (int16_t)((vx + (int16_t)w - 1) - x0);
#if CA_AVR_AGGR
  // Non-flipped fast path increments the destination x monotonically
  // This keeps the compiler from inserting extra address math in the loop
  if (!hFlip) {
    int16_t lo = (int16_t)-dxBaseN;
    int16_t hi = (int16_t)((wRegion - 1) - dxBaseN);
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    int16_t sx = lo;
    int16_t dx = (int16_t)(dxBaseN + lo);
    int16_t j  = (int16_t)(sx >> 1);
    // If starting on odd sx, do lower nibble first
    if (sx & 1) {
      const uint8_t b = rd8(src + j);
      const uint8_t ni = (uint8_t)(b & 0x0F);
      if (ni) s_back[dx] = rd16(pal565 + ni);
      ++sx; ++dx; ++j;
    }
    const int16_t jEnd = (int16_t)(hi >> 1);
    for (; j <= jEnd; ++j) {
      const uint8_t b = rd8(src + j);
      uint8_t ni = (uint8_t)(b >> 4); if (ni) s_back[dx] = rd16(pal565 + ni);
      ++dx;
      ni = (uint8_t)(b & 0x0F);       if (ni) s_back[dx] = rd16(pal565 + ni);
      ++dx;
    }
    return;
  }
  // Flipped fast path decrements the destination x monotonically
  // This mirrors the sprite horizontally with minimal conditional logic
  else {
    int16_t lo = (int16_t)(dxBaseF - (int16_t)(wRegion - 1));
    int16_t hi = dxBaseF;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    int16_t sx = lo;
    int16_t dx = (int16_t)(dxBaseF - lo);
    int16_t j  = (int16_t)(sx >> 1);
    // If starting on odd sx, do low nibble first (then move to next byte)
    if (sx & 1) {
      const uint8_t b = rd8(src + j);
      const uint8_t ni = (uint8_t)(b & 0x0F);
      if (ni) s_back[dx] = rd16(pal565 + ni);
      ++sx; --dx; ++j;
    }
    const int16_t jEnd = (int16_t)(hi >> 1);
    int16_t bytes = (int16_t)(jEnd - j + 1);
    while (bytes >= 2) {
      const uint8_t b0 = rd8(src + j);
      const uint8_t b1 = rd8(src + j + 1);
      uint8_t ni;
      ni = (uint8_t)(b0 >> 4); if (ni) s_back[dx] = rd16(pal565 + ni); --dx;
      ni = (uint8_t)(b0 & 0x0F); if (ni) s_back[dx] = rd16(pal565 + ni); --dx;
      ni = (uint8_t)(b1 >> 4); if (ni) s_back[dx] = rd16(pal565 + ni); --dx;
      ni = (uint8_t)(b1 & 0x0F); if (ni) s_back[dx] = rd16(pal565 + ni); --dx;
      j += 2; bytes -= 2;
    }
    if (bytes == 1) {
      const uint8_t b = rd8(src + j);
      uint8_t ni;
      ni = (uint8_t)(b >> 4); if (ni) s_back[dx] = rd16(pal565 + ni); --dx;
      ni = (uint8_t)(b & 0x0F); if (ni) s_back[dx] = rd16(pal565 + ni); --dx;
    }
    return;
  }
#endif
  int16_t sxStart, sxEnd;
  if (!hFlip) {
    int16_t lo = (int16_t)-dxBaseN;
    int16_t hi = (int16_t)((wRegion - 1) - dxBaseN);
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  } else {
    int16_t lo = (int16_t)(dxBaseF - (int16_t)(wRegion - 1));
    int16_t hi = dxBaseF;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  }
  int16_t j = (int16_t)(sxStart >> 1);
  const int16_t jEnd = (int16_t)(sxEnd >> 1);
  for (; j <= jEnd; ++j) {
  const uint8_t b = rd8(src + j);
    const int16_t sx0 = (int16_t)(j << 1);
    const int16_t sx1 = (int16_t)(sx0 + 1);
    if (sx0 >= sxStart) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx0) : (int16_t)(dxBaseN + sx0);
      if ((uint16_t)dx < (uint16_t)wRegion) { const uint8_t ni = (uint8_t)(b >> 4); if (ni) s_back[dx] = rd16(pal565 + ni); }
    }
    if (sx1 <= sxEnd) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx1) : (int16_t)(dxBaseN + sx1);
      if ((uint16_t)dx < (uint16_t)wRegion) { const uint8_t ni = (uint8_t)(b & 0x0F); if (ni) s_back[dx] = rd16(pal565 + ni); }
    }
  }
}

void CA_Blit::composeOver4bppKey_AVR_P(const uint8_t* data, uint16_t w, uint16_t h,
                                       int16_t vx, int16_t vy, uint16_t* paletteRam,
                                       bool hFlip, uint16_t key565,
                                       int16_t y, int16_t x0, int16_t wRegion){
  const int16_t row = (int16_t)(y - vy); if ((uint16_t)row >= h) return;
  const uint16_t bpr = (w + 1) >> 1;
  const uint8_t* src = data + (uint32_t)row * bpr;
  const int16_t dxBaseN = (int16_t)(vx - x0);
  const int16_t dxBaseF = (int16_t)((vx + (int16_t)w - 1) - x0);
#if CA_AVR_AGGR
  if (!hFlip) {
    int16_t lo = (int16_t)-dxBaseN;
    int16_t hi = (int16_t)((wRegion - 1) - dxBaseN);
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    int16_t sx = lo;
    int16_t dx = (int16_t)(dxBaseN + lo);
    int16_t j  = (int16_t)(sx >> 1);
    if (sx & 1) {
      const uint8_t b = rd8(src + j);
      const uint8_t ni = (uint8_t)(b & 0x0F);
      const uint16_t c = paletteRam[ni];
      if (c != key565) s_back[dx] = c;
      ++sx; ++dx; ++j;
    }
    const int16_t jEnd = (int16_t)(hi >> 1);
    for (; j <= jEnd; ++j) {
      const uint8_t b = rd8(src + j);
      uint8_t ni; uint16_t c;
      ni = (uint8_t)(b >> 4); c = paletteRam[ni]; if (c != key565) s_back[dx] = c; ++dx;
      ni = (uint8_t)(b & 0x0F); c = paletteRam[ni]; if (c != key565) s_back[dx] = c; ++dx;
    }
    return;
  }
  else {
    int16_t lo = (int16_t)(dxBaseF - (int16_t)(wRegion - 1));
    int16_t hi = dxBaseF;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    int16_t sx = lo;
    int16_t dx = (int16_t)(dxBaseF - lo);
    int16_t j  = (int16_t)(sx >> 1);
    if (sx & 1) {
      const uint8_t b = rd8(src + j);
      const uint8_t ni = (uint8_t)(b & 0x0F);
      const uint16_t c = paletteRam[ni];
      if (c != key565) s_back[dx] = c;
      ++sx; --dx; ++j;
    }
    const int16_t jEnd = (int16_t)(hi >> 1);
    int16_t bytes = (int16_t)(jEnd - j + 1);
    while (bytes >= 2) {
      const uint8_t b0 = rd8(src + j);
      const uint8_t b1 = rd8(src + j + 1);
      uint8_t ni; uint16_t c;
      ni = (uint8_t)(b0 >> 4); c = paletteRam[ni]; if (c != key565) s_back[dx] = c; --dx;
      ni = (uint8_t)(b0 & 0x0F); c = paletteRam[ni]; if (c != key565) s_back[dx] = c; --dx;
      ni = (uint8_t)(b1 >> 4); c = paletteRam[ni]; if (c != key565) s_back[dx] = c; --dx;
      ni = (uint8_t)(b1 & 0x0F); c = paletteRam[ni]; if (c != key565) s_back[dx] = c; --dx;
      j += 2; bytes -= 2;
    }
    if (bytes == 1) {
      const uint8_t b = rd8(src + j);
      uint8_t ni; uint16_t c;
      ni = (uint8_t)(b >> 4); c = paletteRam[ni]; if (c != key565) s_back[dx] = c; --dx;
      ni = (uint8_t)(b & 0x0F); c = paletteRam[ni]; if (c != key565) s_back[dx] = c; --dx;
    }
    return;
  }
#endif
  int16_t sxStart, sxEnd;
  if (!hFlip) {
    int16_t lo = (int16_t)-dxBaseN;
    int16_t hi = (int16_t)((wRegion - 1) - dxBaseN);
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  } else {
    int16_t lo = (int16_t)(dxBaseF - (int16_t)(wRegion - 1));
    int16_t hi = dxBaseF;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  }
  int16_t j = (int16_t)(sxStart >> 1);
  const int16_t jEnd = (int16_t)(sxEnd >> 1);
  for (; j <= jEnd; ++j) {
  const uint8_t b = rd8(src + j);
    const int16_t sx0 = (int16_t)(j << 1);
    const int16_t sx1 = (int16_t)(sx0 + 1);
    if (sx0 >= sxStart) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx0) : (int16_t)(dxBaseN + sx0);
      if ((uint16_t)dx < (uint16_t)wRegion) { const uint8_t ni = (uint8_t)(b >> 4); const uint16_t c = paletteRam[ni]; if (c != key565) s_back[dx] = c; }
    }
    if (sx1 <= sxEnd) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx1) : (int16_t)(dxBaseN + sx1);
      if ((uint16_t)dx < (uint16_t)wRegion) { const uint8_t ni = (uint8_t)(b & 0x0F); const uint16_t c = paletteRam[ni]; if (c != key565) s_back[dx] = c; }
    }
  }
}

void CA_Blit::composeOver4bppKeyIdx_AVR_P(const uint8_t* data, uint16_t w, uint16_t h,
                                          int16_t vx, int16_t vy, uint16_t* paletteRam,
                                          bool hFlip, uint8_t keyIndex,
                                          int16_t y, int16_t x0, int16_t wRegion){
  const int16_t row = (int16_t)(y - vy); if ((uint16_t)row >= h) return;
  const uint16_t bpr = (w + 1) >> 1;
  const uint8_t* src = data + (uint32_t)row * bpr;
  const int16_t dxBaseN = (int16_t)(vx - x0);
  const int16_t dxBaseF = (int16_t)((vx + (int16_t)w - 1) - x0);
#if CA_AVR_AGGR
  if (!hFlip) {
    int16_t lo = (int16_t)-dxBaseN;
    int16_t hi = (int16_t)((wRegion - 1) - dxBaseN);
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    int16_t sx = lo;
    int16_t dx = (int16_t)(dxBaseN + lo);
    int16_t j  = (int16_t)(sx >> 1);
    if (sx & 1) {
      const uint8_t b = rd8(src + j);
      const uint8_t ni = (uint8_t)(b & 0x0F);
      if (ni != keyIndex) s_back[dx] = paletteRam[ni];
      ++sx; ++dx; ++j;
    }
    const int16_t jEnd = (int16_t)(hi >> 1);
    for (; j <= jEnd; ++j) {
      const uint8_t b = rd8(src + j);
      uint8_t ni;
      ni = (uint8_t)(b >> 4); if (ni != keyIndex) s_back[dx] = paletteRam[ni]; ++dx;
      ni = (uint8_t)(b & 0x0F); if (ni != keyIndex) s_back[dx] = paletteRam[ni]; ++dx;
    }
    return;
  }
  else {
    int16_t lo = (int16_t)(dxBaseF - (int16_t)(wRegion - 1));
    int16_t hi = dxBaseF;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    int16_t sx = lo;
    int16_t dx = (int16_t)(dxBaseF - lo);
    int16_t j  = (int16_t)(sx >> 1);
    if (sx & 1) {
      const uint8_t b = rd8(src + j);
      const uint8_t ni = (uint8_t)(b & 0x0F);
      if (ni != keyIndex) s_back[dx] = paletteRam[ni];
      ++sx; --dx; ++j;
    }
    const int16_t jEnd = (int16_t)(hi >> 1);
    int16_t bytes = (int16_t)(jEnd - j + 1);
    while (bytes >= 2) {
      const uint8_t b0 = rd8(src + j);
      const uint8_t b1 = rd8(src + j + 1);
      uint8_t ni;
      ni = (uint8_t)(b0 >> 4); if (ni != keyIndex) s_back[dx] = paletteRam[ni]; --dx;
      ni = (uint8_t)(b0 & 0x0F); if (ni != keyIndex) s_back[dx] = paletteRam[ni]; --dx;
      ni = (uint8_t)(b1 >> 4); if (ni != keyIndex) s_back[dx] = paletteRam[ni]; --dx;
      ni = (uint8_t)(b1 & 0x0F); if (ni != keyIndex) s_back[dx] = paletteRam[ni]; --dx;
      j += 2; bytes -= 2;
    }
    if (bytes == 1) {
      const uint8_t b = rd8(src + j);
      uint8_t ni;
      ni = (uint8_t)(b >> 4); if (ni != keyIndex) s_back[dx] = paletteRam[ni]; --dx;
      ni = (uint8_t)(b & 0x0F); if (ni != keyIndex) s_back[dx] = paletteRam[ni]; --dx;
    }
    return;
  }
#endif
  int16_t sxStart, sxEnd;
  if (!hFlip) {
    int16_t lo = (int16_t)-dxBaseN;
    int16_t hi = (int16_t)((wRegion - 1) - dxBaseN);
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  } else {
    int16_t lo = (int16_t)(dxBaseF - (int16_t)(wRegion - 1));
    int16_t hi = dxBaseF;
    if (lo < 0) lo = 0;
    if (hi > (int16_t)w - 1) hi = (int16_t)w - 1;
    if (hi < lo) return;
    sxStart = lo; sxEnd = hi;
  }
  int16_t j = (int16_t)(sxStart >> 1);
  const int16_t jEnd = (int16_t)(sxEnd >> 1);
  for (; j <= jEnd; ++j) {
  const uint8_t b = rd8(src + j);
    const int16_t sx0 = (int16_t)(j << 1);
    const int16_t sx1 = (int16_t)(sx0 + 1);
    if (sx0 >= sxStart) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx0) : (int16_t)(dxBaseN + sx0);
      if ((uint16_t)dx < (uint16_t)wRegion) { const uint8_t ni = (uint8_t)(b >> 4); if (ni != keyIndex) s_back[dx] = paletteRam[ni]; }
    }
    if (sx1 <= sxEnd) {
      const int16_t dx = hFlip ? (int16_t)(dxBaseF - sx1) : (int16_t)(dxBaseN + sx1);
      if ((uint16_t)dx < (uint16_t)wRegion) { const uint8_t ni = (uint8_t)(b & 0x0F); if (ni != keyIndex) s_back[dx] = paletteRam[ni]; }
    }
  }
}
#endif