#include "DrawSetup.h"
#include <TouchScreen.h>
#include <SPI.h>
#include <string.h>

namespace { // global
  // Single pointer to the sketch-owned TFT
  MCUFRIEND_kbv* s_tft = nullptr;
}

namespace CA_Draw {

void init(CA_BlitConfig& cfg, MCUFRIEND_kbv* tft_in, uint8_t scale){
  s_tft = tft_in;                           // bind to the single TFT
  cfg.tft     = tft_in;
  cfg.scale   = scale;
  cfg.screenW = tft_in->width();
  cfg.screenH = tft_in->height();
  s_tft->fillScreen(0x0000);
}

void drawBackground(const CA_BlitConfig& cfg){
  // Paint the entire screen using the same 2× BG scanline composer used by the renderer
  MCUFRIEND_kbv* t = cfg.tft;
  uint16_t* pal = getBgPaletteRAM();     // 256-entry palette in RAM (fast)

  const int16_t W = cfg.screenW;
  const int16_t H = cfg.screenH;

  t->startWrite();
  t->setAddrWindow(0, 0, W - 1, H - 1);
  bool first = true;

  for (int16_t y = 0; y < H; ++y){
    CA_Blit::composeBGLine_160to320_quads_P(
      BG8_q0, BG8_q1, BG8_q2, BG8_q3,
      BG8_W, BG8_H, BG8_cw, BG8_ch,
      pal,
      y, /*x0=*/0, /*w=*/W
    );
    CA_Blit::pushLinePhysicalNoAddr(cfg, W, first);
    first = false;
  }
  t->endWrite();
}

void restoreRect(const CA_BlitConfig& cfg, int16_t vx, int16_t vy, int16_t vw, int16_t vh){
  // Restore a screen-space rect using the 2× BG scanline composer (no 1:1 blit)
  if (vw <= 0 || vh <= 0) return;

  int16_t x0 = vx; if (x0 < 0) x0 = 0;
  int16_t y0 = vy; if (y0 < 0) y0 = 0;
  int16_t x1 = vx + vw; if (x1 > cfg.screenW)  x1 = cfg.screenW;
  int16_t y1 = vy + vh; if (y1 > cfg.screenH) y1 = cfg.screenH;
  if (x1 <= x0 || y1 <= y0) return;

  MCUFRIEND_kbv* t = cfg.tft;
  uint16_t* pal = getBgPaletteRAM();

  const int16_t W = x1 - x0;

  t->startWrite();
  t->setAddrWindow(x0, y0, x1 - 1, y1 - 1);
  bool first = true;

  for (int16_t y = y0; y < y1; ++y){
    CA_Blit::composeBGLine_160to320_quads_P(
      BG8_q0, BG8_q1, BG8_q2, BG8_q3,
      BG8_W, BG8_H, BG8_cw, BG8_ch,
      pal,
      y, /*x0=*/x0, /*w=*/W
    );
    CA_Blit::pushLinePhysicalNoAddr(cfg, W, first);
    first = false;
  }
  t->endWrite();
}


// ---- palette caches ----
struct PalEntry { const uint16_t* src; uint16_t ram[16]; };
static PalEntry s_pals[16]; // palette cache
static uint8_t s_palN=0;

uint16_t* ensurePaletteRAM(const uint16_t* palProgmem){
  for (uint8_t i=0;i<s_palN;++i) if (s_pals[i].src == palProgmem) return s_pals[i].ram;
  uint8_t slot = (s_palN < (uint8_t)(sizeof(s_pals)/sizeof(s_pals[0]))) ? s_palN++ : (uint8_t)(sizeof(s_pals)/sizeof(s_pals[0]) - 1);
  s_pals[slot].src = palProgmem;
  memcpy_P(s_pals[slot].ram, palProgmem, sizeof(s_pals[slot].ram));
  return s_pals[slot].ram;
}

static uint16_t s_bgPal[256]; static bool s_bgLoaded=false;
uint16_t* getBgPaletteRAM(){
  if (!s_bgLoaded){ for (uint16_t i=0;i<256;++i) s_bgPal[i] = pgm_read_word(&BG8_pal565[i]); s_bgLoaded=true; }
  return s_bgPal;
}

// ---- touch (unchanged baseline) ----
#define YP A3
#define XM A2
#define YM 9
#define XP 8
#define TS_MINX 71
#define TS_MAXX 907
#define TS_MINY 94
#define TS_MAXY 931

static TouchScreen ts = TouchScreen(XP, YP, XM, YM, 300);

bool getTouchScreen(int16_t &sx, int16_t &sy) {
  auto p = ts.getPoint();
  pinMode(XM, OUTPUT); pinMode(YP, OUTPUT);
  if (p.z < 150 || p.z > 1000) return false;
  sx = map(p.y, TS_MINX, TS_MAXX, 0, s_tft->width());
  sy = map(p.x, TS_MINY, TS_MAXY, 0, s_tft->height());
  return true;
}

void restoreUI() {
  s_tft->fillScreen(0x0000);
  s_tft->setCursor(10,10); s_tft->setTextColor(0xFFFF); s_tft->setTextSize(2);
  s_tft->print(F("Exited CastAway"));
}

} // namespace CA_Draw
