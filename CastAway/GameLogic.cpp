#include "GameLogic.h"
#include "DrawSetup.h"
#include "FishAI.h"
#include "LUT.h"
#include <string.h>
#include <avr/pgmspace.h>

namespace {
inline int16_t clamp16(int16_t v, int16_t a, int16_t b){ return lutClamp16(v,a,b); }
}

// =================== Runtime trackers ===================
static uint8_t  s_prevState         = 0xFF;
static uint32_t s_prevTickMs        = 0;
static uint32_t s_inBandMs          = 0;
static uint32_t s_burstUntilMs      = 0;
static int8_t   s_burstForce        = 0;
static uint32_t s_reelEaseUntilMs   = 0;   // grace after REEL starts
static uint32_t s_msgExpireAt       = 0;   // HUD auto-clear
static bool     s_inited            = false;
static uint32_t s_blockBitesUntilMs = 0;   // boot grace

// ---- Fight drift (moving sweet-spot) ----
static int16_t  s_targetDrift     = 0;     // ± added to target
static int8_t   s_targetDriftDir  = +1;    // ± step direction
static uint32_t s_nextDriftFlipMs = 0;     // flip time

// ---- Tap interaction tracking ----
static uint32_t s_lastTapMs       = 0;
static uint8_t  s_tapCombo        = 0;     // grows with taps, decays without taps
static uint32_t s_nextComboDecay  = 0;     // schedule decay when idle

// =================== Tunables (feel) ===================
#ifndef CA_TAP_RECENT_MS
#define CA_TAP_RECENT_MS    240     // window where a tap is "recent"
#endif
#ifndef CA_TAP_MAX_COMBO
#define CA_TAP_MAX_COMBO    10
#endif

// Per-tap immediate punch (scale 0..1000)
#ifndef CA_TAP_FAST_BOOST    // taps closer than 90ms
#define CA_TAP_FAST_BOOST    20
#endif
#ifndef CA_TAP_MED_BOOST     // 90..169ms
#define CA_TAP_MED_BOOST     26
#endif
#ifndef CA_TAP_SLOW_BOOST    // >=170ms (paced rhythm rewarded a bit more)
#define CA_TAP_SLOW_BOOST    34
#endif

// Holding & tapping biases
#ifndef CA_HOLD_BIAS_UP
#define CA_HOLD_BIAS_UP      18     // steady lift just by holding (stronger than before)
#endif
#ifndef CA_TAP_COMBO_GAIN
#define CA_TAP_COMBO_GAIN     6     // extra short-lived lift from recent tapping
#endif
#ifndef CA_NOTAP_DRAG
#define CA_NOTAP_DRAG         0     // extra downward lean when holding but not tapping
#endif

// Damping (higher numerator = less decay meaning it will drop off much less)
#ifndef CA_DAMP_EASE_NUM
#define CA_DAMP_EASE_NUM       3
#endif
#ifndef CA_DAMP_EASE_DEN
#define CA_DAMP_EASE_DEN       4     // 3/4 during early ease
#endif
#ifndef CA_DAMP_FIGHT_NUM
#define CA_DAMP_FIGHT_NUM      5
#endif
#ifndef CA_DAMP_FIGHT_DEN
#define CA_DAMP_FIGHT_DEN      6     // 5/6 during fight (keeps momentum)
#endif

// Velocity caps (allow strong up; limit harsh dives)
#ifndef CA_UP_VEL_CAP
#define CA_UP_VEL_CAP         46
#endif
#ifndef CA_DOWN_VEL_CAP
#define CA_DOWN_VEL_CAP      -50
#endif

// Fish, luck, bursts (toned down so taps dominate)
#ifndef CA_FISH_PULL_DIV
#define CA_FISH_PULL_DIV       7     // was ~6, but 7 allows for a weaker fish force
#endif
#ifndef CA_LUCK_RANGE_SHRINK
#define CA_LUCK_RANGE_SHRINK   1     // keep narrow luck
#endif
#ifndef CA_BURST_FORCE
#define CA_BURST_FORCE        10     // gentler shove
#endif
#ifndef CA_BURST_MS_BASE
#define CA_BURST_MS_BASE     150
#endif

// Success band timings (unchanged)
#ifndef CA_EARLY_BAND_MS
#define CA_EARLY_BAND_MS    1500
#endif
#ifndef CA_TIGHTEN_BAND_MS
#define CA_TIGHTEN_BAND_MS  3500
#endif

// ====================================== IO / Messages ======================================
bool CA_Logic::readTouch(bool& tap, bool& hold, int16_t& sx, int16_t& sy, CA_GameState& gs){
  bool down = CA_Draw::getTouchScreen(sx, sy);
  tap  = (!gs.stylusWasDown && down);
  hold = ( gs.stylusWasDown &&  down);
  if (down && !gs.holding) gs.holdStart = millis();
  gs.holding = down; gs.stylusWasDown = down;
  return down;
}

void CA_Logic::clearMessages(CA_GameState& gs){
  gs.msg.l0[0] = 0;
}

void CA_Logic::pushMessage(CA_GameState& gs, const char* m){
  strncpy(gs.msg.l0, m, sizeof(gs.msg.l0)-1);
  gs.msg.l0[sizeof(gs.msg.l0)-1] = 0;
  s_msgExpireAt = millis() + 2200;
}

void CA_Logic::pushMessage(CA_GameState& gs, const __FlashStringHelper* m){
  strncpy_P(gs.msg.l0, (PGM_P)m, sizeof(gs.msg.l0)-1);
  gs.msg.l0[sizeof(gs.msg.l0)-1] = 0;
  s_msgExpireAt = millis() + 2200;
}

// ====================================== Core Step ==========================================
void CA_Logic::step(CA_GameState& gs, const CA_BlitConfig& cfg){
  uint32_t nowMs = millis();

  // One-time init (boot grace to suppress phantom first bite)
  if (!s_inited) {
    s_inited = true;
    s_blockBitesUntilMs = nowMs + 1200;
  }

  // Auto-clear top-right after TTL
  if (gs.msg.l0[0] && nowMs >= s_msgExpireAt) {
    clearMessages(gs);
  }

  // State transition handling
  if (gs.state != s_prevState) {
    if (gs.state == GS_IDLE || gs.state == GS_DRIFT) {
      gs.tension    = 0;
      gs.tensionVel = 0;
      s_inBandMs    = 0;
      s_prevTickMs  = nowMs;
      s_burstUntilMs = 0;
      s_burstForce   = 0;
    }
    s_prevState = gs.state;
  }

  // Input
  bool tap=false, hold=false; int16_t sx=0, sy=0;
  (void)readTouch(tap, hold, sx, sy, gs);

  switch (gs.state) {

    case GS_IDLE:
    case GS_DRIFT: {
      if (gs.activeBiter >= 0) {
        if (nowMs < s_blockBitesUntilMs) { gs.activeBiter = -1; break; }
        pushMessage(gs, F("Bite!"));
        s_msgExpireAt = nowMs + 1500;

        gs.tension    = 0;
        gs.tensionVel = 0;
        gs.biteStart  = nowMs;
        gs.state      = GS_BITE;
        break;
      }

      // tiny idle sway
      if ((nowMs & 15) == 0) {
        gs.tension = clamp16(gs.tension + (((nowMs >> 4) & 3) - 1), 0, 1000);
      }
    } break;

    case GS_BITE: {
      // Gentle rise to a modest level
      uint32_t biteMs = nowMs - gs.biteStart;
      int16_t target = (biteMs < 1200) ? (int16_t)(biteMs / 6) : 200; // ~0..200 over 1.2s
      if (gs.tension < target) gs.tension += 4;
      else if (gs.tension > target) gs.tension -= 2;
      gs.tension = clamp16(gs.tension, 0, 1000);

      if (tap || hold){
        // enter REEL
        gs.reelStart  = nowMs;
        gs.tension    = 520;                 // start a bit higher to avoid early sink
        gs.tensionVel = 0;
        gs.state      = GS_REEL;
        clearMessages(gs);

        // init reel helpers
        s_reelEaseUntilMs = nowMs + 500;    // short grace
        s_prevTickMs = nowMs;
        s_inBandMs = 0;
        s_burstUntilMs = 0;
        s_burstForce = 0;

        // reset tap meta
        s_lastTapMs = 0;
        s_tapCombo  = 0;
        s_nextComboDecay = 0;
      } else if (nowMs - gs.biteStart > 4500) {
        gs.activeBiter = -1;
        gs.tension     = 0;
        gs.tensionVel  = 0;
        gs.state       = GS_IDLE;
        pushMessage(gs, F("Missed bite"));
        s_msgExpireAt  = nowMs + 1500;
      }
    } break;

    case GS_REEL: {
      const uint32_t nowMs2 = millis();
      const bool easing = (nowMs2 < s_reelEaseUntilMs);

      // ---- TAP: big immediate lift + combo ----
      if (tap) {
        uint32_t dt  = nowMs2 - s_lastTapMs;
        uint8_t boost = (dt < 90) ? CA_TAP_FAST_BOOST
                        : (dt < 170 ? CA_TAP_MED_BOOST : CA_TAP_SLOW_BOOST);
        // direct lift + velocity pulse
        gs.tension    = clamp16(gs.tension + boost, 0, 1000);
        gs.tensionVel += (int16_t)boost + (int16_t)(s_tapCombo * 2);

        s_lastTapMs = nowMs2;
        if (s_tapCombo < CA_TAP_MAX_COMBO) s_tapCombo++;
        s_nextComboDecay = nowMs2 + 260; // keep combo hot for a short window
      }

      // Combo decay if no recent taps
      if (s_tapCombo && nowMs2 >= s_nextComboDecay && (nowMs2 - s_lastTapMs) > CA_TAP_RECENT_MS) {
        s_nextComboDecay = nowMs2 + 120;
        s_tapCombo--;
      }

      const bool recentTap = (s_lastTapMs && (nowMs2 - s_lastTapMs) <= CA_TAP_RECENT_MS);

      // ---- Moving sweet spot (slightly calmer) ----
      if (nowMs2 >= s_nextDriftFlipMs) {
        s_targetDriftDir  = -s_targetDriftDir;
        s_nextDriftFlipMs = nowMs2 + 560 + (nowMs2 & 0x1FF);
      }
      s_targetDrift += s_targetDriftDir * 1;
      if (s_targetDrift >   80) s_targetDrift =   80;
      if (s_targetDrift <  -80) s_targetDrift =  -80;

      // Base targets
      const int16_t baseHigh = 730;
      const int16_t baseLow  = 260;
      uint32_t held_ms = gs.holding ? (nowMs2 - gs.holdStart) : 0;
      int32_t  creep   = (int32_t)held_ms / 18;  // unchanged
      if (creep > 120) creep = 120; if (creep < 0) creep = 0;

      int16_t baseTarget = gs.holding ? (int16_t)(baseHigh + (int16_t)creep) : baseLow;
      int16_t target     = (int16_t)(baseTarget + (s_targetDrift >> 1)); // ±40
      int16_t error      = target - gs.tension;

      // Fish & luck (gentler)
      int16_t fishPull = easing ? 0 : (int16_t)((((nowMs2>>4)&63) - 32) / CA_FISH_PULL_DIV);
      int16_t luck     = easing ? 0 : (int16_t)(((nowMs2*1103515245u + 12345u) >> 30) - CA_LUCK_RANGE_SHRINK);

      // Occasional bursts (smaller)
      int16_t burst = 0;
      if (!easing) {
        if (nowMs2 >= s_burstUntilMs) {
          if (((nowMs2>>6) & 0x3F) == 0) {
            s_burstForce   = ((nowMs2>>5)&1) ? +CA_BURST_FORCE : -CA_BURST_FORCE;
            s_burstUntilMs = nowMs2 + CA_BURST_MS_BASE + (nowMs2 & 0x7F);
          } else s_burstForce = 0;
        }
        burst = (nowMs2 < s_burstUntilMs) ? s_burstForce : 0;
      } else {
        s_burstForce = 0;
      }

      // ---- Controller: make reeling stronger, drop-off less when holding ----
      int16_t accel;
      if (easing) {
        accel = (error >> 1);              // stronger than before during grace
      } else if (gs.holding && recentTap) {
        accel = (error);                   // full error → fast correction while tapping
      } else if (gs.holding) {
        accel = (error >> 2);              // holding w/out taps still climbs
      } else {
        accel = (error >> 1);              // not holding: moderate correction to fall toward baseLow
      }

      // Bias terms
      int16_t biasUp   = (gs.holding ? CA_HOLD_BIAS_UP : 0)
                       + (recentTap ? (int16_t)(s_tapCombo * CA_TAP_COMBO_GAIN) : 0);
      int16_t biasDown = gs.holding ? 4 : (int16_t)(20 + (gs.tension / 14)); // holding drops less; released drops fast
      int16_t noTapDrag = (gs.holding && !recentTap) ? CA_NOTAP_DRAG : 0;

      // Integrate velocity (reduced damping)
      if (easing) {
        gs.tensionVel = (int16_t)(( (int32_t)gs.tensionVel * CA_DAMP_EASE_NUM) / CA_DAMP_EASE_DEN)
                        + accel + biasUp - biasDown;
      } else {
        gs.tensionVel = (int16_t)(( (int32_t)gs.tensionVel * CA_DAMP_FIGHT_NUM) / CA_DAMP_FIGHT_DEN)
                        + accel + fishPull + luck + burst
                        + biasUp - (biasDown + noTapDrag);
      }
      if (gs.tensionVel >  CA_UP_VEL_CAP) gs.tensionVel =  CA_UP_VEL_CAP;
      if (gs.tensionVel <  CA_DOWN_VEL_CAP) gs.tensionVel =  CA_DOWN_VEL_CAP;

      gs.tension += gs.tensionVel;
      gs.tension = clamp16(gs.tension, 0, 1000);

      // Success band (unchanged timing; keeps tensioned battles interesting)
      const uint32_t tReel = nowMs2 - gs.reelStart;
      int16_t bandLo = 500, bandHi = 670;
      if (tReel < CA_EARLY_BAND_MS)      { bandLo -= 120; bandHi += 120; }
      else if (tReel > CA_TIGHTEN_BAND_MS){ bandLo += 30;  bandHi -= 30;  }

      uint32_t dt = (s_prevTickMs==0) ? 0 : (nowMs2 - s_prevTickMs);
      s_prevTickMs = nowMs2;

      if (gs.tension >= bandLo && gs.tension <= bandHi) s_inBandMs += dt;
      else { // decay progress more gently (keeps near-wins alive)
        uint32_t dec = dt / 3;
        s_inBandMs = (s_inBandMs > dec) ? (s_inBandMs - dec) : 0;
      }

      // Fail/Win conditions
      const bool canSnap = (nowMs2 >= gs.reelStart + 1100);
      if (gs.tension > 950 && canSnap) {
        pushMessage(gs, F("Line snapped")); s_msgExpireAt = nowMs2 + 1500;
        gs.tension = 0; gs.tensionVel = 0;
        gs.activeBiter = -1; gs.state = GS_FAIL;
      }
      else if (tReel >= 4200 && s_inBandMs >= 2400) {
        pushMessage(gs, F("Caught!")); s_msgExpireAt = nowMs2 + 1500;
        gs.caughtCount++;
        gs.tension = 0; gs.tensionVel = 0;
        gs.activeBiter = -1; gs.state = GS_CATCH;
      }
      else if (tReel > 11000 && s_inBandMs < 1600) {
        pushMessage(gs, F("It slipped")); s_msgExpireAt = nowMs2 + 1500;
        gs.tension = 0; gs.tensionVel = 0;
        gs.activeBiter = -1; gs.state = GS_FAIL;
      }
    } break;

    case GS_CATCH:
    case GS_FAIL:
      gs.state = GS_IDLE;
      break;

    default: break;
  }
}
