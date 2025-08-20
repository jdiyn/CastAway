// CastAwayDemo.ino — boot the TFT and run CastAway at 1x sprites, 2x background

#include <Arduino.h>
#include <MCUFRIEND_kbv.h>
#include "CastAway.h"
#include "DrawSetup.h"

CastAwayGame game;
MCUFRIEND_kbv tft; // primary tft

// ---- Arduino ----
void setup() {
  uint16_t ID;
  tft.reset();
  ID = tft.readID();
  if (ID == 0xD3D3) ID = 0x9486;
  tft.begin(ID);
  tft.setRotation(1);              // 320x240
  tft.fillScreen(0x0000);
  game.begin(&tft);                // game will init at scale=1, bg drawn at 2x by CA_Draw
//  game.setFishCount(7, /*reinit=*/true); // set the initial fish count in the lake and spread them out
//   game.setCaughtCount(9, false);  // Uncomment to test with a starting score
}

void loop() {
  game.tick();
  if (!game.isActive()) { CA_Draw::restoreUI(); while (1) delay(1000); }
}
