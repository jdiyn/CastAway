#ifndef CAST_AWAY_H
#define CAST_AWAY_H

#include <Arduino.h>
#include <MCUFRIEND_kbv.h>
#include "Blitter.h"     // low-level scanline compositors / push helpers
#include "DrawSetup.h"   // TFT init, palettes, background helpers
#include "Anim.h"        // CA_Frame4 / CA_Anim4 definitions
#include "FishAI.h"      // fish structs and update
#include "GameLogic.h"   // game state + FSM
#include "Render.h"      // scanline renderer (world + foreground + UI)

// Assets (4bpp frames + palettes in PROGMEM)
#include "assets/BOAT.h"
#include "assets/MAN1.h"
#include "assets/MAN2.h"
#include "assets/MAN3.h"
#include "assets/MAN4.h"
#include "assets/MAN5.h"
#include "assets/MAN6.h"

#include "assets/FISHINGROD1.h"
#include "assets/FISHINGROD2.h"
#include "assets/FISHINGROD3.h"
#include "assets/FISHINGROD4.h"
#include "assets/FISHINGROD5.h"
#include "assets/FISHINGROD6.h"
#include "assets/FISHINGROD7.h"

#include "assets/FISH1.h"
#include "assets/FISH2.h"

// -----------------------------------------------------------------------------
// CastAwayGame
// Single facade the sketch talks to. Owns state, updates gameplay each tick,
// and enqueues draw work (sprites/UI) to CARender. Nothing here pushes pixels;
// all physical writes happen in CARender::renderFrame().
// -----------------------------------------------------------------------------
class CastAwayGame {
public:
  void begin(MCUFRIEND_kbv* tft);     // wire up TFT, init background, seed game state, paint first frame
  void tick();                        // one frame: update AI/FSM, queue sprites/UI, render dirty
  bool isActive() const { return active; }
  
  // Starting number of fish to spawn at begin
  // Set this before calling begin
  uint8_t startFishCount = 6;

  // Set number of fish caught (score). Works before or after begin().
  void setCaughtCount(uint16_t n, bool showHudMessage = true);

  // Change active fish count at runtime. If reinit=true, fish are re-seeded
  // and spread out; if false, we keep existing fish and only change the active
  // count window. Safe to call before or after begin().
  void setFishCount(uint8_t n, bool reinit = true);
  uint8_t getFishCount() const { return fishParams.count; }

private:
  // ---- lifetime / plumbing ----
  bool           active = false;      // when false, loop() can exit to a splash etc.
  CA_BlitConfig blitCfg;             // TFT pointer + screenW/H + scale (copied into renderer)
  CA_GameState  gs;                  // positions, input flags, scores, FSM bits (see GameLogic.h)
  CA_Render      renderer;           // scanline renderer with world/foreground/UI layers

  // HUD refresh flags
  bool forceCaughtHudRefresh = false;
  uint16_t initCaughtCount = 0;      // applied on begin() so pre-begin setter persists

  // ---- animations ----
  CA_Anim4  manAnim;                  // current man animation set (points to MAN_IDLE)
  uint32_t   manAnimStart = 0;        // epoch for manAnim timing
  bool       manFlip = false;         // currently unused; placeholder for turning avatar

  // ---- fish ----
  static const uint8_t FMAX = 20;     // Supports up to 20 fish (tuned for Mega2560 SRAM)
  CA_Fish       fish[FMAX];          // individual fish runtime state
  CA_FishParams fishParams;          // bounds, waterline, etc.
  CA_Anim4      fishAnim;             // shared swim anim set (2 frames)

  // ---- mini endgame (empty lake) ----
  bool endgameTriggered = false;      // once true, animate fish fly-away and prompt
  uint32_t endgameStartMs = 0;        // when the sequence began

  // ---- rod ----
  const CA_Anim4* rodAnim = nullptr;  // either ROD_IDLE or ROD_PULL (or bend-by-tension override)
  uint32_t         rodAnimStart = 0;  // epoch for rodAnim timing

  // ---- HUD change tracking (to keep UI dirty as small as possible) ----
  int16_t  prevTension = -1;          // last tension (0..1000); -1 forces first paint
  char     prevMsg[22];               // last status line (fits 5x7 text width)
  int16_t  prevCaught = -1;           // last fish counter

  // ---- lure idle jitter (adds life without per-frame RNG) ----
  int8_t   s_lureJitter     = 0;      // vertical jitter offset
  uint32_t s_nextJitterMs   = 0;      // next time to change vertical jitter
  int8_t   s_lureJitterX    = 0;      // horizontal jitter offset
  uint32_t s_nextJitterXMs  = 0;      // next time to change horizontal jitter

  // ---- FPS sampling (cheap) ----
  uint32_t fpsWindowStart = 0;
  uint16_t fpsFrames      = 0;
  uint8_t  fpsValue       = 0;
  char     fpsBuf[8]      = "0fps";

  // ---- palette caches ----
  uint16_t* boatPalCached = nullptr;  // boat has a single frame → cache its 16 colors once

  // ---- tiny PRNG for quick effects ----
  uint32_t rng = 0xC0DEAAAAu;         // xorshift-ish; good enough for jitter and spawning
  inline uint16_t rnd(){ rng^=rng<<7; rng^=rng>>9; rng^=rng<<8; return (uint16_t)rng; }
};

// -----------------------------------------------------------------------------
// AnimTables
// Static, compile-time animation definitions that stitch together PROGMEM frames
// Each CNC_Anim4 is { frames[], frameCount, msPerFrame, loop }. Frames themselves
// carry the PROGMEM data pointers and dimensions. These are read-only tables
// -----------------------------------------------------------------------------
namespace AnimTables {

  // Man idle: 6-frame loop, ~2.2 fps (450 ms per frame)
  static const CA_Frame4 MAN_FRAMES[6] = {
    { MAN1_data, MAN1_pal565, MAN1_W, MAN1_H },
    { MAN2_data, MAN2_pal565, MAN2_W, MAN2_H },
    { MAN3_data, MAN3_pal565, MAN3_W, MAN3_H },
    { MAN4_data, MAN4_pal565, MAN4_W, MAN4_H },
    { MAN5_data, MAN5_pal565, MAN5_W, MAN5_H },
    { MAN6_data, MAN6_pal565, MAN6_W, MAN6_H },
  };
  static const CA_Anim4 MAN_IDLE = { MAN_FRAMES, 6, 750, 1 };

  // Rod idle: subtle sway, 3 frames @ 250 ms
  static const CA_Frame4 ROD_IDLE_FR[3] = {
    { FISHINGROD1_data, FISHINGROD1_pal565, FISHINGROD1_W, FISHINGROD1_H },
    { FISHINGROD2_data, FISHINGROD2_pal565, FISHINGROD2_W, FISHINGROD2_H },
    { FISHINGROD3_data, FISHINGROD3_pal565, FISHINGROD3_W, FISHINGROD3_H },
  };
  static const CA_Anim4 ROD_IDLE = { ROD_IDLE_FR, 3, 250, 1 };

  // Rod pull: bend frames selected by tension (not strictly time based)
  static const int reel_frame_count = 4;  // must match ROD_PULL_FR length
  static const CA_Frame4 ROD_PULL_FR[reel_frame_count] = {
    { FISHINGROD4_data, FISHINGROD4_pal565, FISHINGROD4_W, FISHINGROD4_H },
    { FISHINGROD5_data, FISHINGROD5_pal565, FISHINGROD5_W, FISHINGROD5_H },
    { FISHINGROD6_data, FISHINGROD6_pal565, FISHINGROD6_W, FISHINGROD6_H },
    { FISHINGROD7_data, FISHINGROD7_pal565, FISHINGROD7_W, FISHINGROD7_H },
  };
  static const CA_Anim4 ROD_PULL = { ROD_PULL_FR, reel_frame_count, 350, 1 };

  // Fish swim: 2-frame loop, flip-flop @ 120 ms per frame
  static const CA_Frame4 FISH_FRAMES[2] = {
    { FISH1_data, FISH1_pal565, FISH1_W, FISH1_H },
    { FISH2_data, FISH2_pal565, FISH2_W, FISH2_H },
  };
  static const CA_Anim4 FISH_SWIM = { FISH_FRAMES, 2, 120, 1 };
}

#endif
