#ifndef CA_FISH_AI_H
#define CA_FISH_AI_H

#include <Arduino.h>
#include "Blitter.h"     // CA_BlitConfig (screen dims / TFT)
// Forward-declare animation type to avoid pulling heavy headers here
struct CA_Anim4;

// -----------------------------------------------------------------------------
// CA_Fish
// Runtime state for a single fish. Positioning is kept in a logical water
// coordinate system, but we also cache the computed on-screen draw info each
// tick (drawX/Y, curFrame, flip) so the renderer doesn’t recompute it
// -----------------------------------------------------------------------------
struct CA_Fish {
  // --- simulation space (water region, not full screen) ---
  int16_t x, y;                  // logical position inside underwater region (see CA_FishParams)

  // --- dirty tracking (previous frame’s drawn rect in physical/screen space) ---
  int16_t prevRectX, prevRectY;  // top-left of the last frame’s fish bounding box (screen space)
  int16_t prevRectW, prevRectH;  // width/height of that box (used to restore background / mark dirty)

  // --- movement ---
  int8_t  vx;                    // signed horizontal velocity in logical units (pixels per step)
  uint8_t sp;                    // speed tier / animation pace hint (0..255)
  uint32_t nextFlipAt;           // millis() time when we next consider flipping direction

  // --- simple AI state ---
  uint8_t mood;                  // coarse brain state (see enum below)
  uint8_t ai;                    // substate / controller ID (implementation-specific)
  uint16_t cd;                   // cooldown / timer (ticks or ms bucket depending on logic)

  // --- animation timers ---
  uint16_t phase;                // phase accumulator (swim wiggle, lure approach curve, etc.)
  uint32_t animStart;            // epoch for current sprite animation selection

  // --- cached per-tick render info (physical/screen space) ---
  // The AI computes these once per tick; the renderer just consumes them.
  int16_t drawX, drawY;          // top-left in screen pixels for this frame
  uint8_t curFrame;              // index into the provided CA_Anim4 frame array
  uint8_t flip;                  // 0 = normal, 1 = horizontal flip

  // NEW: previous pose info for masked/stencil blit
  uint8_t prevFrame;
  uint8_t prevFlip;
  
  // Endgame flying animation
  int16_t endgameStartX, endgameStartY; // Original position when endgame starts
};

// -----------------------------------------------------------------------------
// CA_FishParams
// Shared parameters describing the underwater region the fish swim in.
// All values here are in physical/screen pixels (not logical).
// -----------------------------------------------------------------------------
struct CA_FishParams {
  uint8_t count;                 // number of fish in play (<= array size held by caller)
  int16_t vw, vh;                // underwater region width/height in screen pixels
  int16_t y0;                    // absolute screen Y at which water starts (top of underwater area)
};

// -----------------------------------------------------------------------------
// High-level fish modes used by AI.
// F_SWIM     : free swimming, idle wandering
// F_ATTRACT  : attracted toward the lure but not yet biting
// F_BITE     : actively biting / tugging the lure
// F_FLEE     : run away (post-fail or spook)
// F_FLY      : endgame - flying upward to the sky
// -----------------------------------------------------------------------------
enum : uint8_t { F_SWIM=0, F_ATTRACT=1, F_BITE=2, F_FLEE=3, F_FLY=4 };

// -----------------------------------------------------------------------------
// CA_FishOps namespace: lifecycle + per-frame update/draw glue
// -----------------------------------------------------------------------------
namespace CA_FishOps {

// Initialize an array of fish with positions/velocities seeded from `seed`
// `p` defines the underwater region in screen coordinates (vw, vh, y0)
void init(CA_Fish* f, const CA_FishParams& p, uint32_t seed);

// Advance AI and build per-fish draw info; also enqueue dirty for background
// restoration using each fish's previous rect. Returns the new active biter:
//  - >=0 : index of the fish currently biting
//  -  -1 : no active biter
// Parameters:
//   f             : array of fish with size >= p.count
//   p             : underwater region and fish count
//   cfg           : blit config (screen size / TFT ptr)
//   spriteAnim    : animation set used for fish (frames are 4bpp paletted)
//   lureAbsX/Y    : lure position in absolute screen pixels
//   gameState     : current high-level game state (reads only)
//   activeBiter   : previous active biter index or -1
//   now           : millis() timestamp for time-based transitions
int8_t updateAndDraw(CA_Fish* f, const CA_FishParams& p,
                     const CA_BlitConfig& cfg,
                     const CA_Anim4& spriteAnim,
                     int16_t lureAbsX, int16_t lureAbsY,
                     uint8_t gameState, int8_t activeBiter,
                     uint32_t now);

// Set all fish to flying mode for endgame sequence
void setFlyingMode(CA_Fish* f, const CA_FishParams& p, uint32_t startTime);

// Restore a rectangular region of the underwater background at 2× scaling
// This is typically called for previous fish rects to erase trails
// (vx,vy,w,h are in screen pixels)
void restoreUnderRect(const CA_BlitConfig& cfg, int16_t vx, int16_t vy, int16_t w, int16_t h);

} // namespace CA_FishOps

#endif
