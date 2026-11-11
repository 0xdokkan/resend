#pragma once

#include "audio/synth.hpp"

#include <memory>
#include <string>

namespace resend::audio {

// Where the synthesizer's samples go: the sound card through miniaudio
// (docs/decisions/0005-audio-output.md), or nowhere. A machine without a
// sound card, a build without miniaudio and the tests get the null output,
// and resend behaves the same with either.
class Output {
public:
    virtual ~Output() = default;
    // True when samples reach a device.
    virtual bool audible() const = 0;
    // What was opened, or why nothing was: for the about box and the logs.
    virtual std::string description() const = 0;
};

std::unique_ptr<Output> null_output(std::string why = "no sound output");
// The default device, playing `synth` until the output is destroyed; the
// null output if there is none.
std::unique_ptr<Output> open_output(Synth& synth);

} // namespace resend::audio
