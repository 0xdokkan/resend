#pragma once

#include "audio/synth.hpp"

#include <span>

// The sounds of the cues (src/app/cues.hpp), as rows of notes: short,
// quiet, and in one key, so that a busy network sounds like a tune and
// not like an alarm clock. Only the storm guard is meant to stand out.
namespace resend::audio {

// The pentatonic scale the hops climb, from C5.
inline constexpr float scale[] = {523.25f, 587.33f, 659.25f, 783.99f, 880.0f, 1046.5f, 1174.66f, 1318.51f};

Sound hop(std::span<const int> degrees);  // a chord of degrees of the scale
Sound ask();                              // an ARP request: up a fourth
Sound answer();                           // its reply: down again
Sound thud();                             // a frame stops: low, with a little noise
Sound resolve();                          // a ping's reply: an octave up
Sound fail();                             // a ping fails: down a fourth
Sound alarm();                            // the storm guard: two bright notes, twice
Sound goal();                             // a goal met
Sound win();                              // a lab solved: an arpeggio and its chord
Sound click();                            // a button

} // namespace resend::audio
