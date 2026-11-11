#include "audio/synth.hpp"

#include <algorithm>
#include <cmath>

namespace resend::audio {
namespace {
constexpr float attack = 0.008f;  // s
// Each voice's share of the mix, so that a few voices are loud enough; the
// limiter keeps eight at full volume within range.
constexpr float voice_level = 0.35f;
} // namespace

float limit(float sample) {
    const float size = std::abs(sample);
    if (size <= 0.8f) return sample;
    const float knee = 0.8f + 0.2f * std::tanh((size - 0.8f) / 0.2f);
    return std::copysign(std::min(knee, 0.999f), sample);
}

void Synth::play(const Sound& sound, float stretch) {
    stretch = std::clamp(stretch, 0.1f, 4.0f);
    std::lock_guard lock(mutex_);
    for (const auto& note : sound) {
        Voice voice;
        voice.note = note;
        voice.begin = clock_ + static_cast<std::int64_t>(note.start * stretch * sample_rate);
        voice.end = voice.begin + std::max<std::int64_t>(1, static_cast<std::int64_t>(note.length * stretch * sample_rate));
        voice.noise ^= static_cast<std::uint32_t>(voice.begin * 2654435761u);
        if (voice.noise == 0) voice.noise = 1;
        if (voices_.size() >= max_voices) {
            // The oldest goes first.
            const auto oldest = std::min_element(voices_.begin(), voices_.end(), [](const Voice& a, const Voice& b) { return a.begin < b.begin; });
            voices_.erase(oldest);
        }
        voices_.push_back(voice);
    }
}

float Synth::sample(Voice& voice, std::int64_t at) {
    const float t = static_cast<float>(at - voice.begin) / sample_rate;
    const float length = static_cast<float>(voice.end - voice.begin) / sample_rate;
    const float k = std::clamp(t / length, 0.0f, 1.0f);
    // Rise, then an exponential fall to -60 dB at the end.
    const float envelope = (t < attack ? t / attack : 1.0f) * std::exp(-6.9f * k);
    const float frequency = voice.note.slide > 0 ? voice.note.frequency * std::pow(voice.note.slide / voice.note.frequency, k) : voice.note.frequency;
    voice.phase += frequency / sample_rate;
    const bool wrapped = voice.phase >= 1;
    voice.phase -= std::floor(voice.phase);
    float wave = 0;
    switch (voice.note.wave) {
    case Wave::Square: wave = voice.phase < 0.5 ? 1.0f : -1.0f; break;
    case Wave::Triangle: wave = 4.0f * std::abs(static_cast<float>(voice.phase) - 0.5f) - 1.0f; break;
    case Wave::Noise:
        // A new random level each period: higher frequencies hiss, lower rumble.
        if (wrapped || voice.held == 0) {
            voice.noise ^= voice.noise << 13;
            voice.noise ^= voice.noise >> 17;
            voice.noise ^= voice.noise << 5;
            voice.held = static_cast<float>(voice.noise & 0xffff) / 32767.5f - 1.0f;
            if (voice.held == 0) voice.held = 1e-6f;
        }
        wave = voice.held;
        break;
    }
    return wave * envelope * std::clamp(voice.note.volume, 0.0f, 1.0f) * voice_level;
}

void Synth::render(float* out, std::size_t frames) {
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto at = clock_ + static_cast<std::int64_t>(i);
        float mix = 0;
        for (auto& voice : voices_)
            if (at >= voice.begin && at < voice.end) mix += sample(voice, at);
        out[i] = limit(mix * volume_);
    }
    clock_ += static_cast<std::int64_t>(frames);
    std::erase_if(voices_, [&](const Voice& voice) { return voice.end <= clock_; });
}

void Synth::set_volume(float volume) {
    std::lock_guard lock(mutex_);
    volume_ = std::clamp(volume, 0.0f, 1.0f);
}

float Synth::volume() const {
    std::lock_guard lock(mutex_);
    return volume_;
}

std::size_t Synth::voices() const {
    std::lock_guard lock(mutex_);
    return voices_.size();
}

void Synth::stop() {
    std::lock_guard lock(mutex_);
    voices_.clear();
}

} // namespace resend::audio
