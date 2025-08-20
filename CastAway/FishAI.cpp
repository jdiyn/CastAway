#include "FishAI.h"
#include "DrawSetup.h"
#include "Anim.h"
#include "GameLogic.h"
#include "LUT.h"

static inline uint16_t rand16(uint32_t &r){ r^=r<<7; r^=r>>9; r^=r<<8; return (uint16_t)r; }
static int8_t s_prevActiveBiter = -2;

// Fast global PRNG for all game systems (replaces Arduino random())
static uint32_t s_fastRNG = 0xDEADBEEF;
inline uint16_t fastRand() { 
  s_fastRNG ^= s_fastRNG << 7; 
  s_fastRNG ^= s_fastRNG >> 9; 
  s_fastRNG ^= s_fastRNG << 8; 
  return (uint16_t)s_fastRNG; 
}
inline uint16_t fastRandRange(uint16_t max) {
  return fastRand() % max;
}

// ---- AI tuning ----
const int ATTRACT_R = 124;
const int BITE_R    = 54;
const int AVOID_R   = 36;

// Separation (Manhattan distance) and resolve push per axis
static const uint8_t SEPARATE_R      = 12;  // max |dx|+|dy| to repel
static const int8_t  SEPARATE_PUSH_X = 1;   // X nudge per resolve
static const int8_t  SEPARATE_PUSH_Y = 1;   // Y nudge per resolve

// Use bitmasks (power-of-two cadences) to avoid modulo/div
static const uint8_t  WANDER_MASK = 7;   // fire when (phase & 7)==0  (~every 8 steps)
static const uint8_t  SPEED_MASK  = 63;  // fire when (phase & 63)==0 (~every 64 steps)
static const uint8_t  MIN_SP       = 1;
static const uint8_t  MAX_SP       = 3;

static inline int16_t fastAbs16(int16_t v){ return lutAbs16(v); }

void CA_FishOps::init(CA_Fish* fish, const CA_FishParams& p, uint32_t seed){
  uint32_t r = seed ? seed : 0xACE1u;
  for (uint8_t i=0;i<p.count;i++){
    int16_t rx = (p.vw > 16) ? (int16_t)(8 + (rand16(r) % (p.vw - 16))) : (int16_t)(p.vw/2);
    int16_t ry = (p.vh > 12) ? (int16_t)(6 + (rand16(r) % (p.vh - 12))) : (int16_t)(p.vh/2);

    fish[i].x = rx; fish[i].y = ry;

    fish[i].prevRectX = fish[i].prevRectY = 0;
    fish[i].prevRectW = fish[i].prevRectH = 0;

    // Direction + base speed with some variety
    fish[i].vx  = (rand16(r)&1)? 1 : -1;
    fish[i].sp  = (uint8_t)(MIN_SP + (rand16(r) % (MAX_SP - MIN_SP + 1)));

    // Desync flips and frame clocks right from start
    fish[i].nextFlipAt = millis() + 1200 + (rand16(r) % 2200);

    // We'll reuse 'mood' as a stable per-fish random salt in [80..199]
    fish[i].mood = (uint8_t)(80 + (rand16(r) % 120));
    fish[i].ai   = F_SWIM;
    fish[i].cd   = 0;

    // Phase offset desyncs wander and lets us derive per-fish randomness
    fish[i].phase     = rand16(r);
    fish[i].animStart = millis() - (uint32_t)(rand16(r) % 1000);  // spread initial anim offsets

    fish[i].drawX = fish[i].drawY = 0;
    fish[i].curFrame = 0;
    fish[i].flip = 0;
  }
}

void CA_FishOps::setFlyingMode(CA_Fish* f, const CA_FishParams& p, uint32_t startTime){
  for (uint8_t i=0; i<p.count; ++i){
    f[i].ai = F_FLY;
    f[i].vx = 0; // Stop horizontal movement
    f[i].sp = 2; // Set consistent upward speed (2 pixels per frame)
  }
}

void CA_FishOps::restoreUnderRect(const CA_BlitConfig& cfg, int16_t vx, int16_t vy, int16_t w, int16_t h){
  CA_Draw::restoreRect(cfg, vx, vy, w, h);
}

int8_t CA_FishOps::updateAndDraw(CA_Fish* f, const CA_FishParams& p,
                              const CA_BlitConfig& cfg,
                              const CA_Anim4& anim,
                              int16_t lureAbsX, int16_t lureAbsY,
                              uint8_t gameState, int8_t activeBiter,
                              uint32_t now)
{
  // ---- sprite max bounds (for tail-safe erase box) ----
  // Cache frame dimensions to avoid rescanning every tick
  static uint16_t s_cachedMaxW = 0, s_cachedMaxH = 0;
  static const void* s_lastFramePtr = nullptr;
  
  if (anim.frames != s_lastFramePtr) {
    // Frames changed, rescan dimensions
    s_cachedMaxW = 0; s_cachedMaxH = 0;
    for (uint8_t k=0; k<anim.count; ++k) {
      if (anim.frames[k].w > s_cachedMaxW) s_cachedMaxW = anim.frames[k].w;
      if (anim.frames[k].h > s_cachedMaxH) s_cachedMaxH = anim.frames[k].h;
    }
    s_lastFramePtr = anim.frames;
  }
  
  uint16_t maxFw = s_cachedMaxW, maxFh = s_cachedMaxH;

  // Pre-computed flags
  const bool lureInWater   = (lureAbsY >= (p.y0 + 1));
  const bool seekingOK     = (gameState==GS_IDLE || gameState==GS_DRIFT);
  const bool hasActiveBiter= (activeBiter >= 0);
  const bool isReeling     = (gameState == GS_REEL);

  // If a new biter appeared, make others flee once
  if (hasActiveBiter && activeBiter != s_prevActiveBiter){
    for (uint8_t i=0;i<p.count;i++) if (i != (uint8_t)activeBiter){
      CA_Fish &fi = f[i];
      fi.ai = F_FLEE;
      fi.cd = 90 + (now & 63);
      fi.vx = (fi.x < lureAbsX) ? -1 : +1;
      if (fi.sp < MAX_SP) fi.sp++;
      fi.nextFlipAt = now + 2000 + ((now + i*37) & 511);
    }
  }
  s_prevActiveBiter = activeBiter;

  // =====================================================================
  // Spatial buckets along X for separation (O(N) build, small local scans)
  // Skip separation entirely during active bite/reel to save cycles
  // =====================================================================
  const uint8_t N = p.count;
  if (N > 1 && !hasActiveBiter && SEPARATE_R) {
    if (N == 2) {
      // Special case: direct pairwise check for exactly 2 fish (much faster)
      int16_t dx = f[1].x - f[0].x;
      int16_t dy = f[1].y - f[0].y;
      if (fastAbs16(dx) + fastAbs16(dy) <= SEPARATE_R) {
        // Simple push apart
        f[0].x -= SEPARATE_PUSH_X; 
        f[0].y += (f[0].y <= f[1].y ? -SEPARATE_PUSH_Y : +SEPARATE_PUSH_Y);
        f[1].x += SEPARATE_PUSH_X; 
        f[1].y += (f[0].y <= f[1].y ? +SEPARATE_PUSH_Y : -SEPARATE_PUSH_Y);
      }
    } else {
      // Original optimized bucket system for 3+ fish
      // Original optimized bucket system for 3+ fish
      static uint8_t head[24];         // max 24 cells (fits 320px with CELL_W=16)
      static uint8_t nextIdx[32];      // next pointer per fish (N<=32)
      const uint8_t CELL_W = 16;
      uint8_t nCells = (uint8_t)((p.vw + CELL_W - 1) / CELL_W);
      if (nCells > (uint8_t)(sizeof(head))) nCells = (uint8_t)sizeof(head);

      for (uint8_t c=0; c<nCells; ++c) head[c] = 0xFF;
      for (uint8_t i=0;i<N;i++){
        int16_t x = f[i].x; if (x < 0) x = 0; if (x >= p.vw) x = p.vw - 1;
        uint8_t ci = (uint8_t)(x / CELL_W); if (ci >= nCells) ci = nCells - 1;
        nextIdx[i] = head[ci];
        head[ci] = i;
      }

      // Separation: same cell + next cell only
      for (uint8_t c=0; c<nCells; ++c){
        for (uint8_t i=head[c]; i!=0xFF; i=nextIdx[i]){
          // scan neighbors within same cell
          for (uint8_t j=nextIdx[i]; j!=0xFF; j=nextIdx[j]){
            int16_t dx = f[j].x - f[i].x; if (dx < 0) dx = -dx; if (dx > SEPARATE_R) continue;
            int16_t dy = f[j].y - f[i].y; if (dy < 0) dy = -dy;
            if (dx + dy <= SEPARATE_R){
              f[i].x -= SEPARATE_PUSH_X; f[i].y += (f[i].y <= f[j].y ? -SEPARATE_PUSH_Y : +SEPARATE_PUSH_Y);
              f[j].x += SEPARATE_PUSH_X; f[j].y += (f[i].y <= f[j].y ? +SEPARATE_PUSH_Y : -SEPARATE_PUSH_Y);
            }
          }
          // scan next cell (c+1)
          if (c + 1 < nCells){
            for (uint8_t j=head[c+1]; j!=0xFF; j=nextIdx[j]){
              int16_t dx = f[j].x - f[i].x; if (dx < 0) dx = -dx; if (dx > SEPARATE_R) continue;
              int16_t dy = f[j].y - f[i].y; if (dy < 0) dy = -dy;
              if (dx + dy <= SEPARATE_R){
                f[i].x -= SEPARATE_PUSH_X; f[i].y += (f[i].y <= f[j].y ? -SEPARATE_PUSH_Y : +SEPARATE_PUSH_Y);
                f[j].x += SEPARATE_PUSH_X; f[j].y += (f[i].y <= f[j].y ? +SEPARATE_PUSH_Y : -SEPARATE_PUSH_Y);
              }
            }
          }
        }
      }
    }
  }

  // ============================================================
  // One-pass nearest-to-lure (Manhattan) for “someoneCloser”
  // ============================================================
  int16_t bestDist = 32767, secondDist = 32767;
  int8_t  bestIdx  = -1,    secondIdx  = -1;

  const bool needClosest = (lureInWater && seekingOK && !hasActiveBiter);
  if (needClosest){
    for (uint8_t i=0;i<N;i++){
      if (f[i].ai == F_FLY) continue; // Skip flying fish
      int16_t dx = lureAbsX - f[i].x;
      int16_t dy = (lureAbsY - p.y0) - f[i].y;
      int16_t dd = fastAbs16(dx) + fastAbs16(dy);
      if (dd < bestDist){ secondDist = bestDist; secondIdx = bestIdx; bestDist = dd; bestIdx = (int8_t)i; }
      else if (dd < secondDist){ secondDist = dd; secondIdx = (int8_t)i; }
    }
  }  // ---- per-fish update ----
  for (uint8_t i=0;i<p.count;i++){
    CA_Fish &fi = f[i];
    const bool isActiveBiter = (hasActiveBiter && activeBiter == (int8_t)i);

    const uint8_t saltA = (uint8_t)((fi.mood * 29u + (fi.phase >> 3) + i*17u) & 0xFF);
    const uint8_t saltB = (uint8_t)((fi.mood * 53u + (fi.phase >> 5) + i*31u) & 0xFF);

    // Cadences (bitmasks, no modulo)
    if (((fi.phase + saltA) & WANDER_MASK) == 0){
      int8_t turnBias = ((saltB & 3) == 0) ? -1 : ((saltB & 3) == 1) ? +1 : 0;
      int8_t vx = (int8_t)(fi.vx + turnBias);
      if (vx < -1) vx = -1; else if (vx > +1) vx = +1;
      fi.vx = (vx == 0) ? ((saltB & 1) ? +1 : -1) : vx;
    }
    if (((fi.phase + saltB) & SPEED_MASK) == 0){
      uint8_t wiggle = (uint8_t)(1 + ((saltA >> 5) & 1)); // 1 or 2
      int sp = (int)fi.sp + ((saltA & 1) ? +wiggle : -wiggle);
      if (sp < MIN_SP) sp = MIN_SP; else if (sp > MAX_SP) sp = MAX_SP;
      fi.sp = (uint8_t)sp;
    }

    // ---- AI / movement base ----
    if (fi.ai == F_FLY){
      // Endgame: fly upward at consistent speed from current position
      // Simply move upward like normal swimming, but only in Y direction
      fi.y -= fi.sp; // Move upward at normal fish speed
      // Keep horizontal position stable (no drift)
      
    } else if (fi.ai == F_FLEE){
      fi.x += fi.vx * (fi.sp + 1);
      if ((uint8_t)((now + i) & 3) != 0) fi.y += ((now >> 5) & 1) ? +1 : -1;
      if (fi.cd > 0) { fi.cd--; } else { fi.ai = F_SWIM; }
    } else {
      // baseline drift
      fi.x += fi.vx * fi.sp;
      fi.phase += (uint16_t)(3 + fi.sp);

      // slight vertical meander
      if (((fi.phase + saltA) & 31) == 0) fi.y += ((fi.phase >> 5) & 1) ? +1 : -1;

      if (lureInWater && seekingOK && fi.ai != F_BITE && fi.ai != F_FLY) {
        int16_t ad = fastAbs16(lureAbsX - fi.x) + fastAbs16((lureAbsY - p.y0) - fi.y);
        if (ad < ATTRACT_R) {
          if (fi.cd == 0) fi.cd = 40 + ((now + i*11) & 15);
          fi.ai = F_ATTRACT;
        }
      }

      if ((hasActiveBiter || isReeling) && !isActiveBiter && fi.ai != F_FLY) {
        int16_t dx = lureAbsX - fi.x; int16_t dy = (lureAbsY - p.y0) - fi.y;
        int16_t ad = fastAbs16(dx) + fastAbs16(dy);
        if (ad < AVOID_R && fi.ai != F_FLEE) {
          fi.ai = F_FLEE;
          fi.cd = 70 + ((now + i*17) & 31);
          fi.vx = (fi.x < lureAbsX) ? -1 : +1;
          if (fi.sp < MAX_SP) fi.sp++;
        }
      }

      if (fi.ai == F_ATTRACT) {
        if (((now + i + (saltA>>2)) & 1) == 0) fi.x += (lureAbsX > fi.x) ? +1 : -1;
        if (((now + (i<<1) + (saltB>>3)) & 3) == 0) fi.y += ((lureAbsY - p.y0) > fi.y) ? +1 : -1;

        int16_t dx = lureAbsX - fi.x;
        int16_t dy = (lureAbsY - p.y0) - fi.y;
        int16_t dd = fastAbs16(dx) + fastAbs16(dy);        // O(1) “someoneCloser” via global best/second-best
        bool someoneCloser = false;
        if (needClosest){
          if (bestIdx == (int8_t)i) {
            someoneCloser = (secondIdx >= 0) && (secondDist + 6 < dd);
          } else {
            someoneCloser = (bestIdx >= 0) && (bestDist + 6 < dd);
          }
        }
        if (someoneCloser && (now & 1)) {
          if (fi.cd > 0) fi.cd--;
        }

        // Bite check: prefer Manhattan (cheap)
        if (dd <= 10 && activeBiter == -1) {  // ~radius 5
          fi.ai = F_BITE; fi.cd = 140; activeBiter = (int8_t)i;
        } else {
          uint8_t biteGate = (uint8_t)(((now + (i<<1) + saltB) & 3) == 0);
          if (dd <= BITE_R && activeBiter == -1 && biteGate) {
            fi.ai = F_BITE; fi.cd = 140; activeBiter = (int8_t)i;
          } else if (fi.cd > 0) {
            fi.cd--;
          }
        }
      }
      else if (fi.ai == F_BITE) {
        if (!((gameState == GS_BITE || isReeling) && isActiveBiter)) {
          if (fi.cd > 0) { fi.cd--; }
          else { fi.ai = F_SWIM; if (isActiveBiter) activeBiter = -1; }
        }
      }

      // Make flips less likely by narrowing cadence and lengthening intervals
      if (fi.ai != F_BITE && now >= fi.nextFlipAt){
        if ( ((fi.phase + saltA + i*13) & 15) == 0 ) {
          fi.vx = -fi.vx;
          uint16_t jitter = (uint16_t)(1100 + ((saltB * 13u) & 2047));
          fi.nextFlipAt = now + 1700 + jitter; // longer between flips
        } else {
          fi.nextFlipAt = now + 220 + (saltB & 0x7F); // longer debounce
        }
      }
    }

    if (isReeling && isActiveBiter) {
      fi.nextFlipAt = now + 1500;
      if (((now + i) & 1) == 0) fi.x += (fi.vx > 0 ? +1 : -1);
      if (((now + (i<<1)) & 3) == 0) fi.y += ((now >> 4) & 1) ? +1 : -1;
    }

    // ---- capture OLD rect before recomputing frame/coords ----
    int16_t  oldX = fi.drawX;
    int16_t  oldY = fi.drawY;
    uint16_t oldW = anim.frames[fi.curFrame].w;
    uint16_t oldH = anim.frames[fi.curFrame].h;

    // Stash previous pose for renderer
    fi.prevFrame = fi.curFrame;
    fi.prevFlip  = fi.flip;

    // ---- per-fish animation speed & jitter ----
    uint8_t  speedPct = (uint8_t)(80 + (fi.mood % 61));
    uint32_t span     = now - fi.animStart;
    uint32_t jitter   = (uint32_t)((fi.phase & 15));
    uint32_t animNow  = fi.animStart + (span * speedPct) / 100 + jitter;

    // ---- current frame ----
    const uint8_t frameIdx = CA_Anim::frameAt(anim, fi.animStart, animNow);
    const CA_Frame4& fr   = anim.frames[frameIdx];
    const bool hFlip       = (fi.vx < 0);

    // ---- bounds clamp ----
    // Skip bounds clamping for flying fish during endgame
    if (fi.ai != F_FLY) {
      int16_t halfFrW = (int16_t)fr.w / 2;
      int16_t halfFrH = (int16_t)fr.h / 2;

      bool hitLeft  = (fi.x < halfFrW + 2);
      bool hitRight = (fi.x > p.vw - halfFrW - 2);

      if (hitLeft)       { fi.x = halfFrW + 2; fi.vx = +1; fi.nextFlipAt = now + 400 + (saltA & 511); }
      else if (hitRight) { fi.x = p.vw - halfFrW - 2; fi.vx = -1; fi.nextFlipAt = now + 400 + (saltA & 511); }

      if (fi.y < halfFrH + 2)             { fi.y = halfFrH + 2; }
      else if (fi.y > p.vh - halfFrH - 2) { fi.y = p.vh - halfFrH - 2; }

      if (hitLeft || hitRight) {
        fi.y += ((saltB & 1) ? +2 : -2);
        if (fi.y < halfFrH + 2) fi.y = halfFrH + 2;
        if (fi.y > p.vh - halfFrH - 2) fi.y = p.vh - halfFrH - 2;
      }
    }

    // ---- new draw coords ----
    const int16_t newX = fi.x - (int16_t)fr.w/2;
    const int16_t newY = (p.y0 + fi.y) - (int16_t)fr.h/2;

    // ---- tail-proof prev rect ----
    {
      const bool hadPrev = (fi.drawX | fi.drawY);
      if (hadPrev){
        const int16_t oldCX = oldX + (int16_t)(oldW >> 1);
        const int16_t oldCY = oldY + (int16_t)(oldH >> 1);

        const int8_t PADX = 2, PADY = 1;
        int16_t px = oldCX - (int16_t)(maxFw >> 1) - PADX;
        int16_t py = oldCY - (int16_t)(maxFh >> 1) - PADY;
        int16_t pw = (int16_t)maxFw + (PADX << 1);
        int16_t ph = (int16_t)maxFh + (PADY << 1);

        if (px < 0) { pw += px; px = 0; }
        if (py < 0) { ph += py; py = 0; }
        if (px + pw > cfg.screenW) pw = cfg.screenW - px;
        if (py + ph > cfg.screenH) ph = cfg.screenH - py;

        if (pw > 0 && ph > 0){
          fi.prevRectX = px; fi.prevRectY = py;
          fi.prevRectW = pw; fi.prevRectH = ph;
        } else {
          fi.prevRectW = fi.prevRectH = 0;
        }
      } else {
        fi.prevRectW = fi.prevRectH = 0;
      }
    }

    fi.curFrame = frameIdx;
    fi.flip     = hFlip ? 1 : 0;
    fi.drawX    = newX;
    fi.drawY    = newY;
  }

  return activeBiter;
}