#include "Render.h"
#include "Blitter.h"
#include "DrawSetup.h"
#include "assets/BACKGROUND.h"
#include <string.h>

// Fast PRNG for shimmer effects (replaces Arduino random())
static uint32_t s_shimmerRNG = 0xBADC0FFE;
inline uint16_t shimmerRand() { 
  s_shimmerRNG ^= s_shimmerRNG << 7; 
  s_shimmerRNG ^= s_shimmerRNG >> 9; 
  s_shimmerRNG ^= s_shimmerRNG << 8; 
  return (uint16_t)s_shimmerRNG; 
}
inline uint16_t shimmerRandRange(uint16_t max) {
  return shimmerRand() % max;
}

// Masking and tiling paths removed. Stable scanline + dirty rects only

namespace {
  // Shimmer state
  uint32_t s_nextShimmerMs = 0;
  int16_t  s_shimmerY0 = -1;
  uint8_t  s_shimmerH = 0;
  uint8_t  s_shimmerN = 0;
  int16_t  s_shX[16];
  uint8_t  s_shRow[16];
  uint16_t s_shCol[16];
  uint8_t  s_shRows2xH = 0;  // shimmer rows in 2× vertical units (pairs of lines)

  // Small cache to reuse the BG line for vertically paired scanlines during 2× scaling
  // For a given dirty box (fixed x0 and width), y and y+1 share the same srcY (y>>1),
  // so the background line is identical and can be copied instead of recomposed
  static uint16_t s_bgLineCache[320];
}

  inline bool isBlueish(uint16_t c) {
    uint8_t r = (uint8_t)((c >> 11) & 0x1F);
    uint8_t g = (uint8_t)((c >> 5)  & 0x3F);
    uint8_t b = (uint8_t)(c & 0x1F);
    uint8_t thresh = (uint8_t)(r + (g >> 2));
    if (thresh < 10) thresh = 10;
    return b >= thresh;
  }

  // Bright “white-blue”: high overall brightness, still blue-leaning
  inline bool isWhiteBlue(uint16_t c) {
    uint8_t r = (uint8_t)((c >> 11) & 0x1F);
    uint8_t g = (uint8_t)((c >> 5)  & 0x3F);
    uint8_t b = (uint8_t)(c & 0x1F);
    // brightness ~ (2*r + g + 2*b) in [0..187]
    uint16_t bright = (uint16_t)r + r + g + b + b;
    // blue not lower than red, and quite bright
    return (b >= r) && (bright >= 120);
  }

  inline void maybeUpdateShimmer(const CA_BlitConfig& cfg, uint16_t* bgPal) {
    if (!bgPal) return;
    
  // Reduce shimmer update frequency to save cycles (every 6th frame)
  static uint8_t s_shimmerFrameCounter = 0;
  if (++s_shimmerFrameCounter < 6) return;
    s_shimmerFrameCounter = 0;
    
    uint32_t now = millis();
    if (now < s_nextShimmerMs) return;

    // Surface band: 8 px above waterline, 5 rows tall
    int16_t seaTop = (int16_t)((cfg.screenH * 72) / 100);  // Revert to original calculation
    int16_t y0 = (int16_t)(seaTop - 8);
    if (y0 < 0) y0 = 0;
    uint8_t h = 5;
    if ((int32_t)y0 + h > cfg.screenH) h = (uint8_t)(cfg.screenH - y0);
    if (h == 0) { s_shimmerN = 0; s_shimmerY0 = -1; s_shimmerH = 0; s_shRows2xH = 0; s_nextShimmerMs = now + 100; return; }

    s_shimmerY0 = y0;
    s_shimmerH  = h;
    // 2× vertical units (pair two lines into one shimmer row)
    s_shRows2xH = (uint8_t)((h + 1) >> 1); // ceil(h/2)
    if (s_shRows2xH == 0) { s_shimmerN = 0; s_nextShimmerMs = now + 100; return; }

    // Keep shimmer count modest and deterministic per refresh to reduce rng calls
    uint8_t rnd = shimmerRandRange(16);
  s_shimmerN  = (uint8_t)(5 + (rnd & 3)); // 5..8
  if (s_shimmerN > 8) s_shimmerN = 8;     // tighter cap for perf

    for (uint8_t i=0; i<s_shimmerN; ++i) {
      // Align shimmer to even X so we can draw 2 px wide squares cleanly
      uint16_t rx = shimmerRandRange((uint16_t)cfg.screenW);
      s_shX[i] = (int16_t)(rx & ~1);
      // Choose row in 2× units so it repeats for y and y+1 (2×2 block)
      s_shRow[i] = shimmerRandRange(s_shRows2xH);

      // pick a blue-ish or white-blue color from BG palette; fallback to pure blue
      uint16_t c = 0x001F;
      for (uint8_t tries=0; tries<4; ++tries) {
        uint8_t idx = shimmerRandRange(256);
        uint16_t samp = bgPal[idx];
        if (isBlueish(samp) || isWhiteBlue(samp)) { c = samp; break; }
      }
      s_shCol[i] = c;
    }

  s_nextShimmerMs = now + 820 + shimmerRandRange(120);
  }

  inline void applyShimmerLine(int16_t y, int16_t x0, int16_t w) {
    if (s_shimmerN == 0) return;
    if (y < s_shimmerY0 || y >= (s_shimmerY0 + s_shimmerH)) return;
    // Use 2× vertical row index so shimmer appears as 2×2 squares (same on y and y+1)
    uint8_t row2x = (uint8_t)((y - s_shimmerY0) >> 1);
    for (uint8_t i=0; i<s_shimmerN; ++i) {
      if (s_shRow[i] != row2x) continue;
      int16_t dx = (int16_t)(s_shX[i] - x0);
      if ((uint16_t)dx < (uint16_t)w) {
        uint16_t* lb = CA_Blit::lineBuffer();
        auto blend565 = [](uint16_t a, uint16_t b){
          // Cheap 50/50 blend in RGB565
          uint16_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
          uint16_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
          uint16_t r = (uint16_t)((ar + br) >> 1);
          uint16_t g = (uint16_t)((ag + bg) >> 1);
          uint16_t bl= (uint16_t)((ab + bb) >> 1);
          return (uint16_t)((r << 11) | (g << 5) | bl);
        };
        // Draw 2× horizontally (dx and dx+1) with a soft blend over BG
        lb[dx] = blend565(lb[dx], s_shCol[i]);
        if ((uint16_t)(dx + 1) < (uint16_t)w) lb[dx + 1] = blend565(lb[dx + 1], s_shCol[i]);
      }
    }
  }
  // (dead mask/tile helpers removed)

CA_Render::CA_Render() {}

void CA_Render::begin(const CA_BlitConfig* cfg) {
  blitCfg = *cfg;
  clearDirty();
  clearQueues();
}

void CA_Render::beginFrame() {
  clearQueues();
}

// ---- enqueue (unchanged) ----
void CA_Render::addSprite(const CA_Frame4& f, int16_t vx, int16_t vy, bool hFlip,
                          uint16_t* palRam, uint16_t /*key565*/, int16_t z){
  if (sprN < MAX_SPR) {
    const uint8_t keyIdx = CA_Draw::topLeftKeyIndex(f);
    Sprite s = { f, vx, vy, hFlip, palRam, keyIdx, z };
    spr[sprN++] = s;
  }
}

void CA_Render::addSpriteFG(const CA_Frame4& f, int16_t vx, int16_t vy, bool hFlip,
                            uint16_t* palRam, uint16_t /*key565*/, int16_t z){
  if (fgN < MAX_FG) {
    const uint8_t keyIdx = CA_Draw::topLeftKeyIndex(f);
    Sprite s = { f, vx, vy, hFlip, palRam, keyIdx, z };
    fg[fgN++] = s;
  }
}
void CA_Render::markForegroundDirty(){ fgNeedsFullPass = true; }

void CA_Render::addSolid(int16_t rx, int16_t ry, int16_t rw, int16_t rh, uint16_t c, int16_t z){
  if (recN < MAX_REC) rec[recN++] = { rx, ry, rw, rh, c, z, false };
}
void CA_Render::addOutline(int16_t rx, int16_t ry, int16_t rw, int16_t rh, uint16_t c, int16_t z){
  if (recN < MAX_REC) rec[recN++] = { rx, ry, rw, rh, c, z, true };
}
void CA_Render::addHBar(int16_t bx, int16_t by, int16_t bw, int16_t bh, int16_t fillW, uint16_t c, int16_t z){
  if (barN < MAX_BAR) bar[barN++] = { bx, by, bw, bh, fillW, c, z };
}
void CA_Render::addText(const char* s, int16_t tx, int16_t ty, uint16_t c, int16_t z){
  if (txtN >= MAX_TXT) return;
  strncpy(txt[txtN].str, s, sizeof(txt[txtN].str)-1);
  txt[txtN].str[sizeof(txt[txtN].str)-1]=0;
  txt[txtN].tx = tx; txt[txtN].ty = ty; txt[txtN].color565 = c; txt[txtN].z = z;
  ++txtN;
}
void CA_Render::addTextOpaque(const char* s, int16_t tx, int16_t ty, uint16_t fg, uint16_t bg,
                              uint8_t cols, int16_t z){
  if (txtN >= MAX_TXT) return;
  strncpy(txt[txtN].str, s, sizeof(txt[txtN].str)-1);
  txt[txtN].str[sizeof(txt[txtN].str)-1]=0;
  // Do not render beyond the cleared run; clamp to `cols` characters
  if (cols < sizeof(txt[txtN].str)-1) {
    txt[txtN].str[cols] = '\0';
  }
  txt[txtN].tx = tx; txt[txtN].ty = ty; txt[txtN].color565 = fg; txt[txtN].z = z;
  // piggyback extra state using spare bytes in Text (none available),
  // so instead enqueue a solid clear rect followed by normal text
  // Clear width = cols * 6, height = 7 (font). We clear the full reserved span
  addSolid(tx, (int16_t)ty, (int16_t)(cols * 6), (int16_t)7, bg, (int16_t)(z-1));
  ++txtN; // the prior addSolid already queued; keep text via addText below
}
void CA_Render::addDirtyRect(int16_t x, int16_t y, int16_t w, int16_t h){ addUIRect(x,y,w,h); }
void CA_Render::addDirtyWorldRect(int16_t x, int16_t y, int16_t w, int16_t h){ addWorldRect(x,y,w,h); }

// ---- render ----
void CA_Render::renderFrame() {
  // Frame skipping for performance - skip every other frame if needed
  static uint8_t s_frameSkipCounter = 0;
  static bool s_skipEnabled = false;
  
  // Auto-enable frame skipping if performance is poor
  if (++s_frameSkipCounter >= 60) {  // Check every 60 frames
    s_frameSkipCounter = 0;
    // Disable frame skipping for now - let's try other optimizations first
    s_skipEnabled = false;
  }
  
  static bool s_skipThisFrame = false;
  if (s_skipEnabled) {
    s_skipThisFrame = !s_skipThisFrame;
    if (s_skipThisFrame) {
      clearQueues();
      return;  // Skip this frame entirely
    }
  }

  // Early-out if nothing queued and no dirty at all
  bool anyDirty = (wBoxN || uiBoxN);
  if (!fgNeedsFullPass && !anyDirty) { clearQueues(); return; }

  sortSprites(); sortFG(); sortRects(); sortBars(); sortTexts();

  // Clamp existing boxes (round to even boundaries for 2x BG); do not skip small ones
  for (uint8_t i=0;i<wBoxN;++i) {
    if (wbox[i].valid) {
      clampBox(wbox[i].valid, wbox[i].minX,wbox[i].minY,wbox[i].maxX,wbox[i].maxY, blitCfg.screenW, blitCfg.screenH);
      // Round dirty rects to even boundaries for 2x efficiency (keep full area)
      // Round dirty rects to even boundaries for 2x efficiency
      if (wbox[i].valid) {
        wbox[i].minX &= ~1;  // Round down to even
        wbox[i].minY &= ~1;
        wbox[i].maxX = (wbox[i].maxX + 1) & ~1;  // Round up to even
        wbox[i].maxY = (wbox[i].maxY + 1) & ~1;
      }
    }
  }
  for (uint8_t i=0;i<uiBoxN;++i) {
    if (ui[i].valid) {
      clampBox(ui[i].valid, ui[i].minX,ui[i].minY,ui[i].maxX,ui[i].maxY, blitCfg.screenW, blitCfg.screenH);
      // Do not drop tiny UI dirty regions; tension bar/fps text depend on these
    }
  }

  if (!bgPalRam) bgPalRam = CA_Draw::getBgPaletteRAM();
  maybeUpdateShimmer(blitCfg, bgPalRam);

  // Ensure FG area will be painted on first frame if requested
  if (fgNeedsFullPass){
    for (uint8_t i=0;i<fgN;++i){
      const Sprite& s = fg[i];
      addWorldRect(s.vx, s.vy, s.f.w, s.f.h);
    }
    fgNeedsFullPass = false;
  }

  // Clamp again after FG expansion
  for (uint8_t i=0;i<wBoxN;++i) if (wbox[i].valid) clampBox(wbox[i].valid, wbox[i].minX,wbox[i].minY,wbox[i].maxX,wbox[i].maxY, blitCfg.screenW, blitCfg.screenH);

  // Merge UI boxes into world boxes to minimize windows
  for (uint8_t i=0;i<uiBoxN;++i){
    if (!ui[i].valid) continue;
    // Try to merge into an existing world box
    bool merged = false;
    for (uint8_t j=0;j<wBoxN;++j){
      if (!wbox[j].valid) continue;
      if (intersects(wbox[j], ui[i].minX, ui[i].minY, (int16_t)(ui[i].maxX-ui[i].minX), (int16_t)(ui[i].maxY-ui[i].minY))){
        // Unconditionally union when intersecting (prior stable behavior)
        wbox[j].minX = min(wbox[j].minX, ui[i].minX);
        wbox[j].minY = min(wbox[j].minY, ui[i].minY);
        wbox[j].maxX = max(wbox[j].maxX, ui[i].maxX);
        wbox[j].maxY = max(wbox[j].maxY, ui[i].maxY);
        merged = true;
        break;
      }
    }
    if (!merged) {
      if (wBoxN < MAX_WB) {
        // No intersect, but we still need this UI area drawn—add as its own box
        wbox[wBoxN++] = ui[i];
      } else {
        // Fallback: union into the first world box to avoid dropping UI redraw (e.g., tension bar)
        uint8_t j = 0; // choose 0 as conservative union target
        wbox[j].minX = min(wbox[j].minX, ui[i].minX);
        wbox[j].minY = min(wbox[j].minY, ui[i].minY);
        wbox[j].maxX = max(wbox[j].maxX, ui[i].maxX);
        wbox[j].maxY = max(wbox[j].maxY, ui[i].maxY);
      }
    }
  }
  uiBoxN = 0; // all UI handled via merged world boxes

  // Re-round world boxes to even boundaries after UI merges for consistent 2x BG
  for (uint8_t i=0;i<wBoxN;++i) {
    if (wbox[i].valid) {
      // Keep box coverage while aligning to even coordinates
      wbox[i].minX &= ~1;
      wbox[i].minY &= ~1;
      wbox[i].maxX = (wbox[i].maxX + 1) & ~1;
      wbox[i].maxY = (wbox[i].maxY + 1) & ~1;
    }
  }

  // Render each world box (UI already merged)
  MCUFRIEND_kbv* t = blitCfg.tft;
  t->startWrite();
  for (uint8_t bi=0; bi<wBoxN; ++bi){
    const Box& b = wbox[bi]; if (!b.valid) continue;
    const int16_t W = (int16_t)(b.maxX - b.minX); if (W <= 0) continue;
  // Track last composed BG srcY for this box to reuse on the next line when possible
  int16_t lastSrcY = -1;
  bool bgCachedValid = false;

  // Collect visible sprites for this box once
  uint8_t visWIdx[MAX_SPR]; uint8_t visWN = 0;
  uint8_t visFIdx[MAX_FG ]; uint8_t visFN = 0;
    auto rectIntersects = [](int16_t ax,int16_t ay,int16_t aw,int16_t ah,
                             int16_t bx,int16_t by,int16_t bw,int16_t bh)->bool{
      return !(ax+aw<=bx || ay+ah<=by || ax>=bx+bw || ay>=by+bh);
    };
    for (uint8_t i=0;i<sprN;++i){
      const Sprite& s = spr[i];
      if (rectIntersects(s.vx, s.vy, s.f.w, s.f.h, b.minX, b.minY, W, (int16_t)(b.maxY - b.minY))){
        if (visWN < MAX_SPR) visWIdx[visWN++] = i;
      }
    }
    for (uint8_t i=0;i<fgN;++i){
      const Sprite& s = fg[i];
      if (rectIntersects(s.vx, s.vy, s.f.w, s.f.h, b.minX, b.minY, W, (int16_t)(b.maxY - b.minY))){
        if (visFN < MAX_FG) visFIdx[visFN++] = i;
      }
    }

    blitCfg.tft->setAddrWindow(b.minX, b.minY, (int16_t)(b.maxX - 1), (int16_t)(b.maxY - 1));

    bool first = true;
    for (int16_t y = b.minY; y < b.maxY; ++y){
      // Efficient 2× BG: reuse the previous line when it maps to the same srcY
      const int16_t srcY = (int16_t)(y >> 1);
      if (bgCachedValid && srcY == lastSrcY) {
        // Copy cached BG pixels into the current line buffer
        memcpy(CA_Blit::lineBuffer(), s_bgLineCache, (size_t)W * sizeof(uint16_t));
      } else {
        CA_Blit::composeBGLine_160to320_quads_P(
          BG8_q0, BG8_q1, BG8_q2, BG8_q3,
          BG8_W, BG8_H, BG8_cw, BG8_ch,
          bgPalRam, y, b.minX, W
        );
        // Cache for the immediate next line with the same srcY
        memcpy(s_bgLineCache, CA_Blit::lineBuffer(), (size_t)W * sizeof(uint16_t));
        lastSrcY = srcY; bgCachedValid = true;
      }
      applyShimmerLine(y, b.minX, W);

      // World sprites: simple per-line scan (stable)
      for (uint8_t k=0;k<visWN;++k){
        const Sprite& s = spr[visWIdx[k]];
        if (y < s.vy || y >= (int16_t)(s.vy + s.f.h)) continue;
        CA_Blit::composeOver4bppKeyIdx_P(
          s.f.data, s.f.w, s.f.h,
          s.vx, s.vy, s.palRam,
          s.hFlip, s.keyIdx,
          y, b.minX, W
        );
      }
      // FG sprites
      for (uint8_t k=0;k<visFN;++k){
        const Sprite& s = fg[visFIdx[k]];
        if (y < s.vy || y >= (int16_t)(s.vy + s.f.h)) continue;
        CA_Blit::composeOver4bppKeyIdx_P(
          s.f.data, s.f.w, s.f.h,
          s.vx, s.vy, s.palRam,
          s.hFlip, s.keyIdx,
          y, b.minX, W
        );
      }

      // UI overlay
      for (uint8_t i=0;i<recN;++i){ const Rect& r = rec[i]; if (r.isOutline) continue; CA_Blit::composeSolidRectLine(y, b.minX, W, r.rx, r.ry, r.rw, r.rh, r.color565); }
      for (uint8_t i=0;i<recN;++i){ const Rect& r = rec[i]; if (!r.isOutline) continue; CA_Blit::composeRectOutlineLine(y, b.minX, W, r.rx, r.ry, r.rw, r.rh, r.color565); }
      for (uint8_t i=0;i<barN;++i){ const HBar& hb = bar[i]; CA_Blit::composeHBarLine(y, b.minX, W, hb.bx, hb.by, hb.bw, hb.bh, hb.fillW, hb.color565); }
      if (txtN){
        uint16_t* lb = CA_Blit::lineBuffer();
        for (uint8_t j=0;j<txtN;++j){
          const Text& tx = txt[j];
          if (y < tx.ty || y >= tx.ty + 7) continue;
          const uint8_t rowMask = (uint8_t)(1 << (y - tx.ty));
          int16_t cx = tx.tx;
          for (const char* c = tx.str; *c; ++c){
            uint8_t ch = (uint8_t)*c;
            if (ch < 32 || ch > 127){ cx += 6; continue; }
            ch -= 32;
            for (uint8_t col=0; col<5; ++col){
              // Read glyph column directly from PROGMEM to avoid any pointer-decay quirks
              uint8_t colBits = pgm_read_byte(&FONT5x7[ch][col]);
              if (colBits & rowMask){
                int16_t dx = (int16_t)((cx + col) - b.minX);
                if ((uint16_t)dx < (uint16_t)W) lb[dx] = tx.color565;
              }
            }
            cx += 6;
          }
        }
      }

      CA_Blit::pushLinePhysicalNoAddr(blitCfg, W, first);
      first = false;
    }
  }
  t->endWrite();
  clearDirty();
  clearQueues();
}

// ---- utils (unchanged) ----
void CA_Render::clampBox(bool& valid, int16_t& x0,int16_t& y0,int16_t& x1,int16_t& y1, int16_t W,int16_t H){
  if (!valid) return;
  if (x0<0) x0=0; if (y0<0) y0=0; if (x1>W) x1=W; if (y1>H) y1=H;
  if (x1<=x0 || y1<=y0) valid=false;
}
bool CA_Render::intersects(const Box& b, int16_t x,int16_t y,int16_t w,int16_t h){
  if (!b.valid) return false;
  const int16_t x1=x+w, y1=y+h;
  return !(x1<=b.minX || y1<=b.minY || x>=b.maxX || y>=b.maxY);
}
void CA_Render::addUIRect(int16_t x,int16_t y,int16_t w,int16_t h){
  if (w<=0 || h<=0) return;
  // Legacy union
  for (uint8_t i=0;i<uiBoxN;++i){
    if (intersects(ui[i], x,y,w,h)){
      ui[i].minX=min(ui[i].minX,x); ui[i].minY=min(ui[i].minY,y);
      ui[i].maxX=max(ui[i].maxX,(int16_t)(x+w)); ui[i].maxY=max(ui[i].maxY,(int16_t)(y+h));
      return;
    }
  }
  if (uiBoxN<MAX_UIB) ui[uiBoxN++] = { true, x,y,(int16_t)(x+w),(int16_t)(y+h) };
}

void CA_Render::addWorldRect(int16_t x,int16_t y,int16_t w,int16_t h){
  if (w<=0 || h<=0) return;
  // Legacy union
  for (uint8_t i=0;i<wBoxN;++i){
    if (intersects(wbox[i], x,y,w,h)){
      wbox[i].minX=min(wbox[i].minX,x); wbox[i].minY=min(wbox[i].minY,y);
      wbox[i].maxX=max(wbox[i].maxX,(int16_t)(x+w)); wbox[i].maxY=max(wbox[i].maxY,(int16_t)(y+h));
      return;
    }
  }
  if (wBoxN<MAX_WB) {
    wbox[wBoxN++] = { true, x,y,(int16_t)(x+w),(int16_t)(y+h) };
  } else {
    wbox[0].minX = min(wbox[0].minX, x);
    wbox[0].minY = min(wbox[0].minY, y);
    wbox[0].maxX = max(wbox[0].maxX, (int16_t)(x+w));
    wbox[0].maxY = max(wbox[0].maxY, (int16_t)(y+h));
  }
}
void CA_Render::clearDirty(){ wBoxN=0; uiBoxN=0; for(uint8_t i=0;i<MAX_WB;++i) wbox[i].valid=false; for(uint8_t i=0;i<MAX_UIB;++i) ui[i].valid=false; }
void CA_Render::clearQueues(){ sprN=recN=barN=txtN=0; fgN=0; }

// z sorts in place, not in buffer
template<typename T>
void CA_Render::zSort(T* arr, uint8_t n){
  for (uint8_t i=1;i<n;++i){
    T v = arr[i]; uint8_t j = i;
    while (j && arr[j-1].z > v.z){ arr[j] = arr[j-1]; --j; }
    arr[j] = v;
  }
}
void CA_Render::sortSprites(){ zSort(spr, sprN); }
void CA_Render::sortFG     (){ zSort(fg,  fgN ); }
void CA_Render::sortRects  (){ zSort(rec, recN); }
void CA_Render::sortBars   (){ zSort(bar, barN); }
void CA_Render::sortTexts  (){ zSort(txt, txtN); }

// Cheap font!
// 5x7 ASCII (0x20..0x7F)
const uint8_t CA_Render::FONT5x7[96][5] PROGMEM = {
  {0,0,0,0,0},{0,0,0x5f,0,0},{0,7,0,7,0},{0x14,0x7f,0x14,0x7f,0x14},
  {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},
  {0,0x1c,0x22,0x41,0},{0,0x41,0x22,0x1c,0},{0x14,0x08,0x3e,0x08,0x14},{0x08,0x08,0x3e,0x08,0x08},
  {0,0x50,0x30,0,0},{0x08,0x08,0x08,0x08,0x08},{0,0x60,0x60,0,0},{0x20,0x10,0x08,0x04,0x02},
  {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
  {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},
  {0x08,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
  {0x3e,0x41,0x79,0x41,0x3e},{0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
  {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},{0x3e,0x41,0x49,0x49,0x7a},
  {0x7f,0x08,0x08,0x08,0x7f},{0,0x41,0x7f,0x41,0},{0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},
  {0x7f,0x40,0x40,0x40,0x40},{0x7f,0x02,0x04,0x02,0x7f},{0x7f,0x02,0x04,0x08,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
  {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
  {0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},
  {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0,0x7f,0x41,0x41,0},
  {0x02,0x04,0x08,0x10,0x20},{0,0x41,0x41,0x7f,0},{0x04,0x02,0x01,0x02,0x04},{0x80,0x80,0x80,0x80,0x80},
  {0,0x03,0x05,0,0},{0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
  {0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},{0x0c,0x52,0x52,0x52,0x3e},
  {0x7f,0x08,0x04,0x04,0x78},{0,0x44,0x7d,0x40,0},{0x20,0x40,0x44,0x3d,0},{0x7f,0x10,0x28,0x44,0},
  {0,0x41,0x7f,0x40,0},{0x7c,0x08,0x04,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
  {0x7c,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
  {0x04,0x3f,0x44,0x40,0x20},{0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
  {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},{0,0x08,0x36,0x41,0},
  {0,0,0x7f,0,0},{0,0x41,0x36,0x08,0},{0x10,0x08,0x08,0x10,0x08}
};
