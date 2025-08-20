#ifndef CA_ANIM_H
#define CA_ANIM_H

#include <Arduino.h>
#include "Blitter.h"

// A flexible animation of N frames. Each frame is a 4-bpp sprite with its own palette.
struct CA_Frame4 {
  const uint8_t* data;       // PROGMEM 4-bpp
  const uint16_t* pal565;    // PROGMEM palette[16]
  uint16_t w, h;             // in VIRTUAL pixels <<--- important when scaling!
};

struct CA_Anim4 {
  const CA_Frame4* frames;   // array of frames
  uint8_t count;             // number of frames
  uint16_t frameMs;          // per-frame duration
  uint8_t loop : 1;          // loop animation
};

// Advance and get current frame index
namespace CA_Anim {
inline uint8_t frameAt(const CA_Anim4& a, uint32_t startMs, uint32_t nowMs){
  if (a.count==0) return 0;
  uint32_t t = nowMs - startMs;
  uint32_t idx = (t / a.frameMs);
  if (!a.loop && idx >= a.count) return a.count-1;
  return (uint8_t)(idx % a.count);
}
} // namespace

#endif
