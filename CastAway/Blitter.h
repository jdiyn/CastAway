#ifndef CA_BLITTER_H
#define CA_BLITTER_H

#include <Arduino.h>
#include <MCUFRIEND_kbv.h>
#include <avr/pgmspace.h>

// -----------------------------------------------------------------------------
// CA_BlitConfig
// Lightweight bundle of LCD/viewport info shared by all blitters.
//  - tft     : target display (already begun + rotated)
//  - scale   : global sprite scale (usually 1). Background uses its own 2× path.
//  - screenW/H: physical screen size in pixels after rotation.
// -----------------------------------------------------------------------------
struct CA_BlitConfig {
  MCUFRIEND_kbv* tft = nullptr;
  uint8_t  scale  = 1;
  int16_t  screenW = 320;
  int16_t  screenH = 240;
};

// -----------------------------------------------------------------------------
// CA_Blit
// Low-level, allocation-free pixel compositors and push helpers.
// Notes:
//  - "compose*" functions write a single scanline into a shared internal line
//    buffer (s_back[]) inside the .cpp. Call pushLinePhysicalNoAddr() after
//    composing each line to send it to the LCD.
//  - PROGMEM vs RAM palettes:
//      * Functions that take `const uint16_t* pal565` expect a palette that may
//        reside in PROGMEM. Access is slower.
//      * Functions that take `uint16_t* paletteRam` expect a 16-entry palette
//        already copied to RAM (fast). Use CA_Draw::ensurePaletteRAM().
//  - All coordinates are screen-space unless documented otherwise.
// -----------------------------------------------------------------------------
namespace CA_Blit {

  // Opt-in switch for AVR-optimized inner loops
  #ifndef CA_AVR_FAST_BLIT
  #define CA_AVR_FAST_BLIT 1    // Enable fast AVR sprite blitters
  #endif

  // Gate extra-aggressive inner-loop variants (heavier unrolling, fewer checks)
  #ifndef CA_AVR_AGGR
  #define CA_AVR_AGGR 1
  #endif

  // Optional: raw 8080 parallel push path (direct port writes). Default off
  // Enable only on AVR and after configuring the bus pins via setRawBus()
  #ifndef CA_AVR_LCD_RAW
  #define CA_AVR_LCD_RAW 1
  #endif
  // Raw LCD bus removed per perf review; keeping API surface minimal.

  // Compose a 4bpp sprite over the current line buffer (no transparency key)
  // Any non-zero nibble is drawn; index 0 is transparent (skipped)
  // Use composeOver4bppKey_P when you need a color key
  void composeOver4bpp_P(const uint8_t* data, uint16_t w, uint16_t h,
                         int16_t vx, int16_t vy, const uint16_t* pal565,
                         bool hFlip, int16_t y, int16_t x0, int16_t wRegion);

  // Compose a 4bpp sprite over the current line buffer using an RGB565 key color
  // Any pixel whose palette-resolved RGB565 equals key565 is skipped
  //  paletteRam : 16-entry palette in RAM (fast lookups)
  //  key565     : transparent color (often taken from frame's top-left)
  void composeOver4bppKey_P(const uint8_t* data, uint16_t w, uint16_t h,
                            int16_t vx, int16_t vy, uint16_t* paletteRam,
                            bool hFlip, uint16_t key565,
                            int16_t y, int16_t x0, int16_t wRegion);

  // Compose a 4bpp sprite using transparent palette index (0..15)
  void composeOver4bppKeyIdx_P(const uint8_t* data, uint16_t w, uint16_t h,
                               int16_t vx, int16_t vy, uint16_t* paletteRam,
                               bool hFlip, uint8_t keyIndex,
                               int16_t y, int16_t x0, int16_t wRegion);

  // Fill a solid rectangle on the current scanline into s_back[]
  // Only affects the portion intersecting y and [x0, x0+wRegion]
  void composeSolidRectLine(int16_t y, int16_t x0, int16_t wRegion,
                            int16_t rx, int16_t ry, int16_t rw, int16_t rh,
                            uint16_t color565);

  // Draw a 1px outline of a rectangle on the current scanline into s_back[]
  // Only affects the portion intersecting y and [x0, x0+wRegion)
  void composeRectOutlineLine(int16_t y, int16_t x0, int16_t wRegion,
                              int16_t rx, int16_t ry, int16_t rw, int16_t rh,
                              uint16_t color565);

  // Compose a simple left-to-right filled horizontal bar for HUDs.
  // Only affects the portion intersecting y and [x0, x0+wRegion).
  void composeHBarLine(int16_t y, int16_t x0, int16_t wRegion,
                       int16_t bx, int16_t by, int16_t bw, int16_t bh,
                       int16_t fillW, uint16_t color565);

  // ---------------------------------------------------------------------------
  // Background line composer: 160×120 → 320×240 (2× scale)
  // Reconstructs the background scanline y (screen space) using pre-split
  // 8bpp quadrants q0..q3 and a 256-color palette. Writes into s_back[].
  //  w160/h120 : logical BG size (typically 160×120)
  //  cw/ch     : quadrant width/height in pixels (e.g., 80×60)
  //  pal565    : 256-entry palette (PROGMEM or RAM)
  //  y         : destination scanline in screen space (0..screenH-1)
  //  x0,w      : horizontal region within the screen to compose
  // ---------------------------------------------------------------------------
  void composeBGLine_160to320_quads_P(const uint8_t* q0, const uint8_t* q1,
                                      const uint8_t* q2, const uint8_t* q3,
                                      uint16_t w160, uint16_t h120,
                                      uint16_t cw, uint16_t ch,
                                      const uint16_t* pal565,
                                      int16_t y, int16_t x0, int16_t w);

  // Enable AVR-optimized inner loop for BG replicate (2x) when available
#ifndef CA_AVR_BG_FAST
#define CA_AVR_BG_FAST 1
#endif

  // ---------------------------------------------------------------------------
  // Push the composed scanline in s_back[] to the LCD
  // Call after one or more compose* calls for the same y
  //  - The address window must be pre-set to the enclosing rect for all lines
  //  - `first` should be true for the very first push after setAddrWindow()
  //    so the driver can optimize its initial write sequence
  //  - `w` is the exact number of pixels composed for this line (wRegion)
  // ---------------------------------------------------------------------------
  void pushLinePhysicalNoAddr(const CA_BlitConfig& cfg, int16_t w, bool first);

  // Access the current scanline buffer (mutable). No bounds checks.
  uint16_t* lineBuffer();

  // Optional: write a pixel into the current scanline at dx (0..wRegion-1). No bounds checks
  void pokeLinePixel(int16_t dx, uint16_t color565);

  // If enabled, provide AVR-specific variants and remap the generic names
  #if CA_AVR_FAST_BLIT
    void composeOver4bpp_AVR_P(const uint8_t* data, uint16_t w, uint16_t h,
                               int16_t vx, int16_t vy, const uint16_t* pal565,
                               bool hFlip, int16_t y, int16_t x0, int16_t wRegion);
    void composeOver4bppKey_AVR_P(const uint8_t* data, uint16_t w, uint16_t h,
                                  int16_t vx, int16_t vy, uint16_t* paletteRam,
                                  bool hFlip, uint16_t key565,
                                  int16_t y, int16_t x0, int16_t wRegion);
    void composeOver4bppKeyIdx_AVR_P(const uint8_t* data, uint16_t w, uint16_t h,
                                     int16_t vx, int16_t vy, uint16_t* paletteRam,
                                     bool hFlip, uint8_t keyIndex,
                                     int16_t y, int16_t x0, int16_t wRegion);

    #define composeOver4bpp_P       composeOver4bpp_AVR_P
    #define composeOver4bppKey_P    composeOver4bppKey_AVR_P
    #define composeOver4bppKeyIdx_P composeOver4bppKeyIdx_AVR_P
  #endif
} // namespace CA_Blit
#endif
