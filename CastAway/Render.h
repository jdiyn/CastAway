#ifndef CA_RENDER_H
#define CA_RENDER_H

#include <Arduino.h>
#include <MCUFRIEND_kbv.h>
#include <avr/pgmspace.h>
#include <string.h>
#include "Blitter.h"
#include "Anim.h"

/**
 * CA_Render
 * ---------
 * Scanline renderer that composites:
 *   1) Background (2× scaled from 160×120 into 320×240 via BG composer)
 *   2) World sprites (z-sorted amongst themselves)
 *   3) Foreground sprites (z-sorted amongst themselves; always above world)
 *   4) UI primitives (rects, outlines, bars, text) — always top-most
 *
 * Rendering happens only inside "dirty" boxes (screen-space unions), to minimize
 * writes to the LCD. World dirty boxes are usually driven by moving game objects
 * UI dirty boxes are merged into any overlapping world box so their scanlines
 * are pushed once.
 *
 * Key details:
 *  - Coordinates are screen-space pixels (0..screenW/H-1)
 *  - Colors are 16-bit RGB565 (uint16_t)
 *  - Sprites are 4bpp paletted frames (CA_Frame4). Transparency is per-sprite
 *    via a key color (key565). Use CA_Draw::frameKey565(frame, palRAM) to pick
 *    the key from the frame's top-left pixel if you want "topleftkey" semantics
 *  - Foreground sprites do NOT expand dirty regions by themselves, but can be
 *    forced to redraw fully for one frame via markForegroundDirty(), useful on
 *    startup or when the FG set changes
 */
class CA_Render {
public:
  CA_Render();
  void begin(const CA_BlitConfig* cfg);
  void beginFrame();
  void setBgPalette(uint16_t* palRam) { bgPalRam = palRam; }

  // PUBLIC DRAW API (declared in header, defined in Render.cpp)
  void addSprite(const CA_Frame4& f, int16_t vx, int16_t vy, bool hFlip,
                 uint16_t* palRam, uint16_t key565, int16_t z);
  void addSpriteFG(const CA_Frame4& f, int16_t vx, int16_t vy, bool hFlip,
                   uint16_t* palRam, uint16_t key565, int16_t z);
  void markForegroundDirty();

  void addSolid(int16_t rx, int16_t ry, int16_t rw, int16_t rh, uint16_t c, int16_t z);
  void addOutline(int16_t rx, int16_t ry, int16_t rw, int16_t rh, uint16_t c, int16_t z);
  void addHBar(int16_t bx, int16_t by, int16_t bw, int16_t bh, int16_t fillW, uint16_t c, int16_t z);
  void addText(const char* s, int16_t tx, int16_t ty, uint16_t c, int16_t z);
  // Draw text after clearing a fixed background run (cols * 6 px) from (tx,ty)
  void addTextOpaque(const char* s, int16_t tx, int16_t ty, uint16_t fg, uint16_t bg,
                     uint8_t cols, int16_t z);

  void addDirtyRect(int16_t x, int16_t y, int16_t w, int16_t h);
  void addDirtyWorldRect(int16_t x, int16_t y, int16_t w, int16_t h);

  void renderFrame();

  // Expose Sprite so render helpers (in Render.cpp) can reference it
  struct Sprite {
    CA_Frame4 f;
    int16_t vx, vy;
    bool    hFlip;
    uint16_t* palRam;
    uint8_t keyIdx;
    int16_t z;
  };private:
  // ---------------------------- Data structures ----------------------------
  struct Rect { int16_t rx, ry, rw, rh; uint16_t color565; int16_t z; bool isOutline; };
  struct HBar { int16_t bx, by, bw, bh, fillW; uint16_t color565; int16_t z; };
  // Slightly larger to avoid truncation of HUD strings
  struct Text { char str[24]; int16_t tx, ty; uint16_t color565; int16_t z; };
  struct Box  { bool valid; int16_t minX, minY, maxX, maxY; };

  // ---------------------------- Capacity limits ----------------------------
  static constexpr uint8_t MAX_SPR = 16; // reduce the number of sprites (increase if using more fish)
  static constexpr uint8_t MAX_FG  = 2;
  static constexpr uint8_t MAX_REC = 16;
  static constexpr uint8_t MAX_BAR = 4;
  static constexpr uint8_t MAX_TXT = 8;
  static constexpr uint8_t MAX_WB  = 28; // control the number of world boxes
  static constexpr uint8_t MAX_UIB = 8;

  // ---------------------------- Per-frame queues ----------------------------
  Sprite spr[MAX_SPR]; uint8_t sprN=0;
  Sprite fg [MAX_FG ]; uint8_t fgN =0;
  Rect   rec[MAX_REC]; uint8_t recN=0;
  HBar   bar[MAX_BAR]; uint8_t barN=0;
  Text   txt[MAX_TXT]; uint8_t txtN=0;

  // ---------------------------- Dirty region unions ----------------------------
  Box wbox[MAX_WB]; uint8_t wBoxN=0;
  Box ui  [MAX_UIB]; uint8_t uiBoxN=0;

  // ---------------------------- Global state ----------------------------
  CA_BlitConfig blitCfg;
  uint16_t* bgPalRam = nullptr;
  bool fgNeedsFullPass = false;
  
  // ---------------------------- Helpers ----------------------------
  static void clampBox(bool& valid, int16_t& x0,int16_t& y0,int16_t& x1,int16_t& y1, int16_t W,int16_t H);
  static bool intersects(const Box& b, int16_t x,int16_t y,int16_t w,int16_t h);
  void addUIRect(int16_t x, int16_t y, int16_t w, int16_t h);
  void addWorldRect(int16_t x, int16_t y, int16_t w, int16_t h);
  void clearDirty();
  void clearQueues();
  template<typename T> static void zSort(T* arr, uint8_t n);
  void sortSprites(); void sortFG(); void sortRects(); void sortBars(); void sortTexts();

  // 5×7 ASCII
  static const uint8_t FONT5x7[96][5] PROGMEM;
};

#endif
