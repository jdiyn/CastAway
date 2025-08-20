#ifndef CA_GAME_LOGIC_H
#define CA_GAME_LOGIC_H

#include <Arduino.h>
#include "Blitter.h"
#include "DrawSetup.h"

enum : uint8_t { GS_IDLE=0, GS_DRIFT=1, GS_BITE=2, GS_REEL=3, GS_CATCH=4, GS_FAIL=5 };

struct CA_GameState {
  int16_t vw=160, vh=120;

  // boat/man placement
  int16_t boatX=32, boatY=78;
  int16_t manX=0, manY=0, manPrevX=0, manPrevY=0;

  // rod anchor (screen absolute)
  int16_t rodAx=58, rodAy=-10;

  // input
  bool stylusWasDown=false, holding=false;
  uint32_t holdStart=0, reelStart=0, biteStart=0;

  // tension 0..1000
  int16_t tension=300, tensionVel=0;

  // AI
  int8_t activeBiter=-1;

  // catch count
  uint16_t caughtCount=0;

  // message log (top-right)
  CA_Draw::MsgLog msg{ "" };

  uint8_t state = GS_IDLE;
};

namespace CA_Logic {

// input
bool readTouch(bool& tap, bool& hold, int16_t& sx, int16_t& sy, CA_GameState& gs);

// messages
void clearMessages(CA_GameState& gs);
// Overloads to safely accept both flash-resident string literals (F("..."))
// and RAM strings without ambiguity.
void pushMessage(CA_GameState& gs, const __FlashStringHelper* m);
void pushMessage(CA_GameState& gs, const char* m);

// main FSM (no popups, uses message log)
void step(CA_GameState& gs, const CA_BlitConfig& cfg);

} // namespace
#endif
