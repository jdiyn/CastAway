#include "CastAway.h"
using namespace AnimTables;

// Fast PRNG for all game systems (replaces Arduino random())
static uint32_t s_gameRNG = 0xC0DEFACE;
inline uint16_t gameRand() { 
  s_gameRNG ^= s_gameRNG << 7; 
  s_gameRNG ^= s_gameRNG >> 9; 
  s_gameRNG ^= s_gameRNG << 8; 
  return (uint16_t)s_gameRNG; 
}
inline int16_t gameRandRange(int16_t min, int16_t max) {
  return min + (gameRand() % (max - min + 1));
}

  // Fast fish count formatter (replaces snprintf)
void fastFormatFishCount(char* buf, uint16_t count) {
  // Actually shows caught fish; label adjusted to prevent confusion with live fish count
  buf[0] = 'C'; buf[1] = 'a'; buf[2] = 'u'; buf[3] = 'g'; buf[4] = 'h'; buf[5] = 't'; buf[6] = ':'; buf[7] = ' ';
  if (count < 10) {
    buf[8] = '0' + count;
    buf[9] = 0;
  } else if (count < 100) {
    buf[8]  = '0' + (count / 10);
    buf[9]  = '0' + (count % 10);
    buf[10] = 0;
  } else if (count < 1000) {
    buf[8]  = '0' + (count / 100);
    buf[9]  = '0' + ((count / 10) % 10);
    buf[10] = '0' + (count % 10);
    buf[11] = 0;
  } else {
    // Fallback for very large counts
    buf[8]  = '9'; buf[9] = '9'; buf[10] = '9'; buf[11] = '+'; buf[12] = 0;
  }
}

// Background optimization: disable fish rendering for testing
static bool s_disableFishRendering = false;

void CastAwayGame::begin(MCUFRIEND_kbv* tft){
  active = true;
  randomSeed(analogRead(A0));

  CA_Draw::init(blitCfg, tft, 1);
  renderer.begin(&blitCfg);

  CA_Draw::drawBackground(blitCfg);
  renderer.setBgPalette(CA_Draw::getBgPaletteRAM());

  gs.vw = blitCfg.screenW; gs.vh = blitCfg.screenH;

  gs.boatX = gs.vw/2 - (BOAT_W/2) + 16;
  gs.boatY = gs.vh/2 + 41;
  gs.manX  = gs.boatX + BOAT_W/2 - MAN1_W/2 - 8;
  gs.manY  = gs.boatY - MAN1_H + 2;
  gs.rodAx = gs.boatX - 32;
  gs.rodAy = gs.boatY - 13;

  // cache the boat's single frame palette
  boatPalCached = CA_Draw::ensurePaletteRAM(BOAT_pal565);

  manAnim      = MAN_IDLE;
  manAnimStart = millis();
  fishAnim     = FISH_SWIM;
  rodAnim      = &AnimTables::ROD_IDLE;
  rodAnimStart = millis();

  const int16_t seaY0  = (gs.vh * 72) / 100;  // Revert to original calculation
  fishParams.y0 = seaY0;
  fishParams.vw = gs.vw;
  fishParams.vh = gs.vh - seaY0; if (fishParams.vh < 0) fishParams.vh = 0;
  // Use configurable starting fish count, clamped to capacity
  fishParams.count = (startFishCount > FMAX) ? FMAX : startFishCount;
  // Initialize fish array with the chosen count
  CA_FishOps::init(fish, fishParams, rnd());

  gs.tension     = 30;
  gs.caughtCount = initCaughtCount; // honor pre-begin setCaughtCount()
  gs.state = GS_DRIFT;
  gs.activeBiter = -1;
  gs.stylusWasDown = false;
  gs.holding = false;
  CA_Logic::clearMessages(gs);
  
  // Reset endgame state
  endgameTriggered = false;
  endgameStartMs = 0;

  fpsWindowStart = millis();
  fpsFrames = 0; fpsValue = 0;
  strcpy(fpsBuf, "0fps");

  // Initial HUD
  renderer.beginFrame();


  // 1) Queue BOAT as a FOREGROUND sprite now, so markForegroundDirty can see it
  {
    const CA_Frame4 boatF = { BOAT_data, BOAT_pal565, BOAT_W, BOAT_H };
    const int16_t boatX = gs.boatX;
    const int16_t boatY = gs.boatY - BOAT_H + 6;
    const uint16_t boatKey = CA_Draw::frameKey565(boatF, boatPalCached);
    renderer.addSpriteFG(boatF, boatX, boatY, /*hFlip=*/false,
                         boatPalCached, boatKey, /*z within FG*/ 0);
  }

  // HUD widgets
  const uint16_t COL_BG=0x224B, COL_WHITE=0xFFFF;

  // Bottom-left tension bar
  const int tbW=72, tbH=4, tbX=10, tbY=blitCfg.screenH - (tbH + 6);
  int tVal = gs.tension; if (tVal<0) tVal=0; if (tVal>1000) tVal=1000;
  const int bw = (int32_t)tbW * tVal / 1000;

  renderer.addSolid  (tbX-2, tbY-2, tbW+4, tbH+4, COL_BG, +20);
  renderer.addOutline(tbX-2, tbY-2, tbW+4, tbH+4, COL_WHITE, +21);
  renderer.addHBar   (tbX,   tbY,   tbW,   tbH,   bw,      COL_WHITE, +22);
  renderer.addDirtyRect(tbX-2, tbY-2, tbW+4, tbH+4);

  // caught counter
  { const int CW=69, CH=14, CX=4, CY=4;
    char cbuf[16]; fastFormatFishCount(cbuf, gs.caughtCount);
    renderer.addSolid  (CX-2, CY-2, CW+4, CH+4, COL_BG, +20);
    renderer.addOutline(CX-2, CY-2, CW+4, CH+4, COL_WHITE, +21);
    renderer.addText   (cbuf, CX+4, CY+3, COL_WHITE, +22);
    renderer.addDirtyRect(CX-2, CY-2, CW+4, CH+4);
    renderer.addDirtyWorldRect(CX-3, CY-3, CW+6, CH+6);
  }
  prevCaught = gs.caughtCount;

  // ensure full foreground paint on first frame
  renderer.markForegroundDirty();

  renderer.renderFrame();

  prevTension = gs.tension;
  strncpy(prevMsg, gs.msg.l0, sizeof(prevMsg)-1); prevMsg[sizeof(prevMsg)-1]=0;
}
// Set caught (score)
void CastAwayGame::setCaughtCount(uint16_t n, bool showHudMessage){
  if (!active) {
    initCaughtCount = n;
    forceCaughtHudRefresh = true;
    return;
  }
  gs.caughtCount = n;
  forceCaughtHudRefresh = true;
}
// Change active fish count at runtime
void CastAwayGame::setFishCount(uint8_t n, bool reinit){
  uint8_t clamped = (n > FMAX) ? FMAX : n;
  fishParams.count = clamped;
  if (reinit) {
    CA_FishOps::init(fish, fishParams, rnd());
  }
}


// ============================ TICK ===================================
// Game happenings every frame
void CastAwayGame::tick(){
  if (!active) return;

  const uint32_t now = millis();

  // Exit button rect
  const int BXW=40, BXH=16, BXX=blitCfg.screenW-(BXW+6), BXY=blitCfg.screenH-(BXH+6);

  // FPS
  fpsFrames++;
  uint32_t elapsed = now - fpsWindowStart;
  if (elapsed >= 1000) {
    fpsValue = (uint8_t)((fpsFrames * 1000UL) / elapsed);
    fpsFrames = 0; fpsWindowStart = now;
    
    // Fast integer-to-string conversion (avoids snprintf)
    // if (fpsValue < 10) {
    //   fpsBuf[0] = '0' + fpsValue;
    //   fpsBuf[1] = 'f'; fpsBuf[2] = 'p'; fpsBuf[3] = 's'; fpsBuf[4] = 0;
    // } else {
      fpsBuf[0] = '0' + (fpsValue / 10);
      fpsBuf[1] = '0' + (fpsValue % 10);
      fpsBuf[2] = 'f'; fpsBuf[3] = 'p'; fpsBuf[4] = 's'; fpsBuf[5] = 0;
   // } 
  }

  // Lure jitter
  const int16_t baseLureY = gs.rodAy + 55;
  const int16_t baseLureX = gs.rodAx - 8;
  const int16_t jyMin = -15, jyMax = +15;
  const int16_t jxMin = -20, jxMax = +20;
  if (gs.state == GS_IDLE || gs.state == GS_DRIFT) {
    static uint32_t s_nextJitterMs=0, s_nextJitterXMs=0;
    static int8_t s_lureJitter=0, s_lureJitterX=0;
    if (now >= s_nextJitterMs)   { s_lureJitter   = (int8_t)gameRandRange(jyMin, jyMax);   s_nextJitterMs   = now + 2000 + gameRandRange(0, 1999); }
    if (now >= s_nextJitterXMs)  { s_lureJitterX  = (int8_t)gameRandRange(jxMin, jxMax);   s_nextJitterXMs  = now + 2000 + gameRandRange(0, 1999); }
    int16_t lureY = constrain((int16_t)(baseLureY + s_lureJitter),  (int16_t)(baseLureY + jyMin), (int16_t)(baseLureY + jyMax));
    int16_t lureX = constrain((int16_t)(baseLureX + s_lureJitterX), (int16_t)(baseLureX + jxMin), (int16_t)(baseLureX + jxMax));
    (void)lureX; (void)lureY; // currently only used by fish update call below
  }

  // FISH first
  gs.activeBiter = CA_FishOps::updateAndDraw(
      fish, fishParams, blitCfg, fishAnim,
      /*lureX*/ gs.rodAx - 8, /*lureY*/ gs.rodAy + 55,
      gs.state, gs.activeBiter, now);

  // FSM
  CA_Logic::step(gs, blitCfg);


  // RENDERING
  renderer.beginFrame();

  // Fish (conditionally disabled for testing). Fish AI handles endgame flying.
  if (!s_disableFishRendering && fishParams.count) {
    for (uint8_t i=0;i<fishParams.count;++i){
      const uint8_t frameNow  = fish[i].curFrame;
      const CA_Frame4& frNow  = fishAnim.frames[frameNow];
      uint16_t* pal = CA_Draw::ensurePaletteRAM(frNow.pal565);
      const uint16_t key565 = CA_Draw::frameKey565(frNow, pal);

      // Ensure the fish's current on-screen rect is marked dirty so the
      // background is composed under its new position (prevents vanish/ghosts
      // when movement skips out of the previous dirty area).
      // Add a tiny 1px pad for safety.
      {
        // Pad more generously to account for flips and union rounding merges
        int16_t cx = (int16_t)(fish[i].drawX - 2);
        int16_t cy = (int16_t)(fish[i].drawY - 2);
        int16_t cw = (int16_t)(frNow.w + 4);
        int16_t ch = (int16_t)(frNow.h + 4);
        renderer.addDirtyWorldRect(cx, cy, cw, ch);
      }

      renderer.addSprite(
        frNow, fish[i].drawX, fish[i].drawY, (fish[i].flip != 0),
        pal, key565, -10
      );
    }
  }

  // ---- WORLD: man, boat, rod ---- 
  // Reduce man animation frequency to minimize BG recomposition
  static uint8_t s_manUpdateCounter = 0;
  static uint8_t s_cachedManIdx = 0;
  
  if (++s_manUpdateCounter >= 3) {  // Update man animation every 3rd frame
    s_manUpdateCounter = 0;
    s_cachedManIdx = CA_Anim::frameAt(manAnim, manAnimStart, now);
  }
  
  const uint8_t manIdx = s_cachedManIdx;
  const CA_Frame4& manF = manAnim.frames[manIdx];

  // Boat: static foreground sprite (do NOT union into world dirty)
  const CA_Frame4 boatF = { BOAT_data, BOAT_pal565, BOAT_W, BOAT_H };
  const int16_t boatX = gs.boatX;
  const int16_t boatY = gs.boatY - BOAT_H + 6;

  // choose rod anim: ONLY pull during REEL (idle otherwise)
  const CA_Anim4* desiredRod = (gs.state == GS_REEL) ? &AnimTables::ROD_PULL : &AnimTables::ROD_IDLE;
  if (desiredRod != rodAnim) {
    rodAnim = desiredRod;
    rodAnimStart = now;     // reset only on set switch
  }

  // While REELing, nudge the perceived speed a bit by tension; idle stays 1×
  uint32_t fakeNow = now;
  if (gs.state == GS_REEL) {
    const uint32_t elapsed_rod = now - rodAnimStart;
    const uint16_t speedPct = 100 + (uint16_t)((uint32_t)60 * (uint16_t)gs.tension / 1000);
    fakeNow = rodAnimStart + (elapsed_rod * speedPct) / 100;
  }

  // Base rod frame from the current set (or tension bend)
  uint8_t rodAnimIdx = CA_Anim::frameAt(*rodAnim, rodAnimStart, fakeNow);
  const CA_Frame4* rodPtr = &rodAnim->frames[rodAnimIdx];
  if (gs.state == GS_REEL) {
    int ridx = (int)((long)gs.tension * reel_frame_count / 1001);   // 0..4 by tension
    if (gs.tensionVel >  10) ++ridx;                     // pulling up = more bend
    if (gs.tensionVel < -10) --ridx;                     // dropping = relax
    if (ridx < 0) ridx = 0; else if (ridx > reel_frame_count - 1) ridx = reel_frame_count - 1;
    rodPtr = &AnimTables::ROD_PULL_FR[ridx];
  }
  const CA_Frame4& rodF = *rodPtr;

  // === Dirty handling: ONLY man+rod ===
  static int16_t prevMR_x0=0, prevMR_y0=0, prevMR_x1=0, prevMR_y1=0;
  static uint8_t lastManIdx = 0xFF;
  static const CA_Frame4* lastRodPtr = nullptr;

  int16_t mr_x0 = min(gs.manX,      gs.rodAx);
  int16_t mr_y0 = min(gs.manY,      (int16_t)(gs.rodAy - rodF.h + 24));
  int16_t mr_x1 = max((int16_t)(gs.manX + manF.w),
                      (int16_t)(gs.rodAx + rodF.w));
  int16_t mr_y1 = max((int16_t)(gs.manY + manF.h),
                      (int16_t)(gs.rodAy + 24));

  // pad a pixel to catch outlines etc.
  mr_x0 -= 1; mr_y0 -= 1; mr_x1 += 1; mr_y1 += 1;

  // Only mark dirty if pose changed or during REEL
  bool mrChanged = (manIdx != lastManIdx) || (rodPtr != lastRodPtr) || (gs.state == GS_REEL);
  if (mrChanged) {
    // re-draw previous MR box to clean trails
    if (prevMR_x1 > prevMR_x0 && prevMR_y1 > prevMR_y0) {
      renderer.addDirtyWorldRect(prevMR_x0, prevMR_y0, (int16_t)(prevMR_x1 - prevMR_x0), (int16_t)(prevMR_y1 - prevMR_y0));
    }
    // draw current MR box
    renderer.addDirtyWorldRect(mr_x0, mr_y0, (int16_t)(mr_x1 - mr_x0), (int16_t)(mr_y1 - mr_y0));
  }

  // stash for next frame
  prevMR_x0 = mr_x0; prevMR_y0 = mr_y0; prevMR_x1 = mr_x1; prevMR_y1 = mr_y1;
  lastManIdx = manIdx; lastRodPtr = rodPtr;

  // === Enqueue sprites (boat drawn as static foreground with highest Z) ===
  uint16_t* manPal  = CA_Draw::ensurePaletteRAM(manF.pal565);
  uint16_t* boatPal = boatPalCached;
  uint16_t* rodPal  = CA_Draw::ensurePaletteRAM(rodF.pal565);

  // Man (no prev-pose path)
  renderer.addSprite(
    manF, gs.manX, gs.manY, /*hFlip*/ false,
    manPal, CA_Draw::frameKey565(manF, manPal), /*z*/ -5
  );

  // Rod (no prev-pose path)
  {
    const int16_t rodX = gs.rodAx;
    const int16_t rodY = (int16_t)(gs.rodAy - rodF.h + 24);
    renderer.addSprite(
      rodF, rodX, rodY, /*hFlip*/ false,
      rodPal, CA_Draw::frameKey565(rodF, rodPal), /*z*/ +5
    );
  }

  // add the boat to the foreground layer (z = 0 in FG layer)
  renderer.addSpriteFG(boatF, boatX, boatY, false, boatPal, CA_Draw::frameKey565(boatF, boatPal), 0);

  // carry-over fish dirty
  for (uint8_t i=0;i<fishParams.count;++i){
    if (fish[i].prevRectW && fish[i].prevRectH)
      renderer.addDirtyWorldRect(fish[i].prevRectX, fish[i].prevRectY, fish[i].prevRectW, fish[i].prevRectH);
  }

  // ---- HUD Colors ----
  const uint16_t COL_BG=0x224B, COL_WHITE=0xFFFF;

  // ---- HUD: Tension bar ----
  // Thinner bar and slimmer frame
  const int tbW=72, tbH=3, tbX=10, tbY=blitCfg.screenH - (tbH + 6);

  int tVal = gs.tension; if (tVal<0) tVal=0; if (tVal>1000) tVal=1000;
  const int16_t bw = (int16_t)(((int32_t)tbW * (int32_t)tVal) / 1000);  // avoid 16-bit overflow on AVR
  const uint16_t barCol = (tVal > 800) ? 0xF800 : (tVal > 500) ? 0xFFE0 : 0xFFFF;

  // Always enqueue UI primitives so they overlay any world box that crosses here
  renderer.addSolid  (tbX-1, tbY-1, tbW+2, tbH+2, COL_BG,    +300);
  renderer.addOutline(tbX-1, tbY-1, tbW+2, tbH+2, barCol,    +301);

  // During redraw, explicitly clear the bar interior before filling to avoid
  // residual pixels when the dirty region merges oddly with world boxes
  // We do this when we mark the area dirty (below) so it only costs on updates
  // Note: COL_BG is our HUD bg inside the frame; this restore is cheap vs full BG
  static bool s_barNeedsClear = true;
  if (s_barNeedsClear) {
    renderer.addSolid(tbX, tbY, tbW, tbH, COL_BG, +302);
  }
  renderer.addHBar   (tbX,   tbY,   tbW,   tbH,   bw,        barCol, +303);

  // Only mark dirty when value/colour changed (or during reel state)
  static int lastBarW = -1;
  static uint16_t lastBarCol = 0xFFFF;
  static bool firstTime = true;
  if (bw != lastBarW || barCol != lastBarCol || gs.state == GS_REEL || firstTime) {
    // Mark both UI and world dirty around the new slimmer frame
    renderer.addDirtyRect(tbX-2, tbY-2, tbW+4, tbH+4);
    renderer.addDirtyWorldRect(tbX-2, tbY-2, tbW+4, tbH+4);
    lastBarW = bw; lastBarCol = barCol;
    firstTime = false;
    s_barNeedsClear = true;  // ensure interior gets cleared on this update
  }
  else {
    s_barNeedsClear = false;
  }

  // ---- Game Status Message (top-center) ----
  static char s_prevGameMsg[22] = {0};
  static bool s_prevVisible = false;
  
  const bool hasGameMsg = (gs.msg.l0[0] != 0);
  const bool gameMsgChanged = strcmp(s_prevGameMsg, gs.msg.l0) != 0;

  // Compute previous/current rects for dirtying
  auto computeMsgRect = [&](const char* msg, int16_t& rx, int16_t& ry, int16_t& rw, int16_t& rh){
    const int textW = (int)strlen(msg) * 6; // 5x7 font + 1px spacing
    const int msgW  = textW + 1; // +1 for shadow offset coverage
    const int msgH  = 7 + 1;     // +1 for shadow offset coverage
    rx = (int16_t)((blitCfg.screenW - msgW) / 2);
    ry = 6; // a bit lower from top edge
    rw = (int16_t)msgW;
    rh = (int16_t)msgH;
  };

  int16_t prevX=0, prevY=0, prevW=0, prevH=0;
  int16_t currX=0, currY=0, currW=0, currH=0;
  computeMsgRect(s_prevGameMsg, prevX, prevY, prevW, prevH);
  computeMsgRect(gs.msg.l0,     currX, currY, currW, currH);

  // If visibility changed or content changed, mark both old and new areas dirty
  if (gameMsgChanged || (s_prevVisible != hasGameMsg)){
    if (s_prevVisible && prevW>0 && prevH>0) {
      renderer.addDirtyRect(prevX, prevY, prevW, prevH);
    }
    if (hasGameMsg && currW>0 && currH>0) {
      renderer.addDirtyRect(currX, currY, currW, currH);
    }
  }

  // Draw current message text when present
  if (hasGameMsg && currW>0 && currH>0){
    // tiny shadow for readability
  // darker shadow for better legibility on bright BG tiles
  renderer.addText(gs.msg.l0, (int16_t)(currX+1), (int16_t)(currY+1), 0x0008, +121);
    renderer.addText(gs.msg.l0, currX, currY, COL_WHITE, +122);
  }

  // Track for next frame
  strncpy(s_prevGameMsg, gs.msg.l0, sizeof(s_prevGameMsg)-1);
  s_prevGameMsg[sizeof(s_prevGameMsg)-1] = 0;
  s_prevVisible = hasGameMsg;

  // ---- FPS (top-right, reduced size) ----
  const int fpsLen = (int)strlen(fpsBuf);
  const int fpsW = fpsLen * 6 + 8; // just wide enough for FPS text + padding
  const int fpsH = 14;
  const int fpsX = blitCfg.screenW - (fpsW + 6);
  const int fpsY = 4;

  // Always enqueue UI primitives (overlay)
  renderer.addSolid  (fpsX-2, fpsY-2, fpsW+4, fpsH+4, COL_BG,    +100);
  renderer.addOutline(fpsX-2, fpsY-2, fpsW+4, fpsH+4, COL_WHITE, +101);
  renderer.addText   (fpsBuf, fpsX+2, fpsY+3, COL_WHITE, +102);
  
  static uint8_t lastFpsShownHere = 0xFF;
  bool fpsChanged = lastFpsShownHere != fpsValue;
  
  // Only mark dirty when FPS changes
  if (fpsChanged) {
    renderer.addDirtyRect(fpsX-2, fpsY-2, fpsW+4, fpsH+4);
    lastFpsShownHere = fpsValue;
  }

  // ---- Fish count (top-left) ----
  { const int CW=69, CH=14, CX=4, CY=4;
    static uint16_t prevCaught = 0xFFFF;

    renderer.addSolid  (CX-2, CY-2, CW+4, CH+4, COL_BG, +100);
    renderer.addOutline(CX-2, CY-2, CW+4, CH+4, COL_WHITE, +101);
    char cbuf[16]; fastFormatFishCount(cbuf, gs.caughtCount);
    renderer.addText   (cbuf, CX+4, CY+3, COL_WHITE, +102);

    if (forceCaughtHudRefresh || gs.caughtCount != prevCaught) {
      renderer.addDirtyRect(CX-2, CY-2, CW+4, CH+4);
      prevCaught = gs.caughtCount;
      forceCaughtHudRefresh = false;
    }
  }

  // =================== ENDGAME: Empty Lake sequence ===================
  if (!endgameTriggered && gs.caughtCount >= 10) {
    endgameTriggered = true;
    endgameStartMs = now;
    // Set all fish to flying mode
    CA_FishOps::setFlyingMode(fish, fishParams, now);
  }

  if (endgameTriggered) {
    // Check if all fish have flown off screen
    bool allGone = true;
    for (uint8_t i=0;i<fishParams.count;++i){
      if (fish[i].y > -50) { // Still visible or close to visible
        allGone = false;
        break;
      }
    }

    if (allGone) {
      renderer.renderFrame();
      // Modal choice
      // Draw a bordered box with two buttons: Exit and Restart, wait for touch
      MCUFRIEND_kbv* tft = blitCfg.tft;
      tft->fillRect(40, 80, 240, 80, 0x0000);
      tft->drawRect(40, 80, 240, 80, 0xFFFF);
      tft->setTextColor(0xFFFF); tft->setTextSize(1);
      tft->setCursor(60, 88); tft->print(F("Lake emptied! Maybe do"));
      tft->setCursor(60, 102); tft->print(F("something productive?"));

      tft->drawRect(60, 120, 70, 20, 0xFFFF); tft->setCursor(75, 126); tft->print(F("Exit"));
      tft->drawRect(190, 120, 70, 20, 0xFFFF); tft->setCursor(198, 126); tft->print(F("Restart"));

      // Wait for choice (blocking until valid tap)
      int16_t sx=0, sy=0; bool chosen=false; bool exitChosen=false;
      while (!chosen) {
        if (CA_Draw::getTouchScreen(sx, sy)) {
          if (sx>=60 && sx<130 && sy>=120 && sy<140) { exitChosen=true; chosen=true; }
          else if (sx>=190 && sx<260 && sy>=120 && sy<140) { exitChosen=false; chosen=true; }
        }
      }

      if (exitChosen) {
        active = false; CA_Draw::restoreUI(); return;
      } else {
        // Full restart - reset all game state
        endgameTriggered = false;
        endgameStartMs = 0;
        gs.tension = 30; gs.tensionVel = 0; gs.activeBiter = -1; gs.state = GS_DRIFT;
        gs.caughtCount = 0; // Reset score
        gs.holding = false; gs.stylusWasDown = false;
        CA_Logic::clearMessages(gs);
        
        // Reset fish
        CA_FishOps::init(fish, fishParams, rnd());
        
        // Reset animations
        manAnimStart = now;
        rodAnimStart = now;
        
        // Reset FPS tracking  
        fpsWindowStart = now;
        fpsFrames = 0;
        
        // Redraw BG and ensure boat FG is re-enqueued
        CA_Draw::drawBackground(blitCfg);
        
        // Re-add boat to foreground layer (same as in begin())
        {
          const CA_Frame4 boatF = { BOAT_data, BOAT_pal565, BOAT_W, BOAT_H };
          const int16_t boatX = gs.boatX;
          const int16_t boatY = gs.boatY - BOAT_H + 6;
          const uint16_t boatKey = CA_Draw::frameKey565(boatF, boatPalCached);
          renderer.addSpriteFG(boatF, boatX, boatY, /*hFlip=*/false,
                               boatPalCached, boatKey, /*z within FG*/ 0);
        }
        
        renderer.markForegroundDirty();
        
        // Also explicitly dirty the entire boat area to ensure full redraw
        const int16_t boatX = gs.boatX;
        const int16_t boatY = gs.boatY - BOAT_H + 6;
        renderer.addDirtyRect(boatX-2, boatY-2, BOAT_W+4, BOAT_H+4);
        
        // Force HUD refresh
        forceCaughtHudRefresh = true;
      }
    }
  }

  // Normal fish rendering continues even during endgame (fish AI handles flying)
  
  // ---- Exit button (bottom-right) ----
  // Use local buffer to avoid any string literal issues
  char exitStr[8];
  exitStr[0] = 'E'; exitStr[1] = 'x'; exitStr[2] = 'i'; exitStr[3] = 't'; exitStr[4] = 0;
  renderer.addText   (exitStr, BXX+8, BXY+4, COL_WHITE, +1000);
  renderer.addSolid  (BXX-2, BXY-2, BXW+4, BXH+4, COL_BG,    +100);
  renderer.addSolid  (BXX,   BXY,   BXW,   BXH,   COL_BG,    +101);
  renderer.addOutline(BXX-2, BXY-2, BXW+4, BXH+4, COL_WHITE, +102);
  renderer.addDirtyRect(BXX-3, BXY-3, BXW+6, BXH+6);

  renderer.renderFrame();

  // Update HUD trackers
  prevTension = gs.tension;

  // Handle Exit button tap
  {
    bool tap=false, hold=false; int16_t sx=0, sy=0; (void)tap; (void)hold;
    // Read but do not disturb gs.holding flags elsewhere; simple one-shot check
    bool down = CA_Draw::getTouchScreen(sx, sy);
    static bool prevDown=false;
    bool justTapped = (!prevDown && down);
    prevDown = down;
    if (justTapped) {
      if (sx >= BXX && sx < BXX+BXW && sy >= BXY && sy < BXY+BXH) {
        active = false;                 // deactivate game
        CA_Draw::restoreUI();           // hand back to sketch loop
        return;
      }
    }
  }
}
