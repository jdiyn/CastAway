#ifndef CA_DRAW_SETUP_H
#define CA_DRAW_SETUP_H

#include <MCUFRIEND_kbv.h>
#include <avr/pgmspace.h>
#include "Blitter.h"          // CA_BlitConfig + push/compose helpers
#include "Anim.h"             // CA_Frame4 (4bpp frames)
#include "assets/BACKGROUND.h"// BG8_* data (160×120 8bpp tiles/quads)

// -----------------------------------------------------------------------------
// CA_Draw
// Lightweight helpers for wiring the TFT, painting the scaled background, and
// managing small RAM palette caches. This module never enqueues sprites; it only
// provides building blocks used by the game and the renderer
// -----------------------------------------------------------------------------
namespace CA_Draw {

  // Clear to black and print a small "Exited CastAway" message. Used when the game
  // deactivates so the sketch can park on a clean screen
  void restoreUI();

  // Bind a TFT instance to a blit config. Stores the TFT pointer, queries width/
  // height, sets scaling factor (typically 1 for sprites; BG is composed at 2×)
  // Also clears the screen to black.
  void init(CA_BlitConfig& cfg, MCUFRIEND_kbv* tft_in, uint8_t scale=1);

  // Full-screen background paint at 2× using the scanline BG composer
  // Uses getBgPaletteRAM() for fast 256-color lookups
  void drawBackground(const CA_BlitConfig& cfg);

  // Restore a rectangular region of the background at 2× scale. (vx,vy,vw,vh) are
  // in screen pixels. This is used to erase trails (e.g., previous fish rects)
  // without redrawing the entire screen
  void restoreRect(const CA_BlitConfig& cfg, int16_t vx, int16_t vy, int16_t vw, int16_t vh);

  // ---------------- Palette management ----------------
  // Copy a 16-entry RGB565 palette from PROGMEM to RAM and return the RAM pointer
  // The copy is cached; repeated calls with the same source pointer return the
  // same RAM block. Keep this small to avoid SRAM pressure
  uint16_t* ensurePaletteRAM(const uint16_t* palProgmem); // 16 entries copied to RAM

  // Return the 256-entry background palette in RAM. Lazily populated from
  // BG8_pal565 on first call. Kept in a single static buffer
  uint16_t* getBgPaletteRAM();                             // 256 entries in RAM

  // ---------------- Transparency key helpers ----------------
  // Utility to derive a per-frame transparent key using the top-left pixel of
  // a 4bpp sprite frame. The first source byte holds the first two pixels:
  // high nibble = pixel (x=0), low nibble = pixel (x=1). We use the high nibble.
  inline uint8_t  topLeftKeyIndex(const CA_Frame4& f) { return pgm_read_byte(f.data) >> 4; }

  // Convert that palette index into an RGB565 color using the provided 16-entry
  // RAM palette. Pass this as key565 when enqueuing sprites to the renderer.
  inline uint16_t frameKey565(const CA_Frame4& f, const uint16_t* palRam){ return palRam[topLeftKeyIndex(f)]; }

  // ---------------- Minimal UI / input helpers ----------------
  // Simple blocking touch read (maps to current TFT width/height). Returns true
  // if a valid touch was detected and writes screen coordinates into (sx, sy)
  bool getTouchScreen(int16_t &sx, int16_t &sy);


  // Small msg log used by the HUD (fits 5×7 font width and keeps RAM small)
  struct MsgLog { char l0[22]; };

} // namespace CADraw

#endif
