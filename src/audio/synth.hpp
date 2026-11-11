#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// The sounds of resend (G5): a small synthesizer with square, triangle and
// noise voices. A sound is a few rows of parameters, never an audio file;
// the cues that choose them are in src/app/cues.hpp. Nothing here knows of
// the network or of a sound card: src/audio/output.hpp plays what render()
// writes.
namespace resend::audio {

constexpr int sample_rate = 48000;
// Voices sounding at once; a new note takes the place of the oldest.
constexpr std::size_t max_voices = 8;

enum class Wave : std::uint8_t { Square, Triangle, Noise };

// One voice of a sound: it starts `start` seconds after the sound, rises in
// 8 ms and dies away over `length`, sliding to `slide` Hz if that is not 0.
struct Note {
    Wave wave = Wave::Square;
    float frequency = 440;
    float slide = 0;
    float start = 0;
    float length = 0.1f;
    float volume = 0.5f;  // of a voice's share, 0 to 1
    bool operator==(const Note&) const = default;
};
using Sound = std::vector<Note>;

// Thread-safe: play() and set_volume() from the interface, render() from
// the output's own thread.
class Synth {
public:
    // Plays `sound` from the next sample rendered; `stretch` scales its
    // times, below 1 to shorten it at high playback speeds.
    void play(const Sound& sound, float stretch = 1);
    // Writes `frames` mono samples, from -1 to 1 and never beyond.
    void render(float* out, std::size_t frames);
    void set_volume(float volume);  // 0 to 1
    float volume() const;
    // Voices sounding or waiting to start.
    std::size_t voices() const;
    // Silences everything at once.
    void stop();

private:
    struct Voice {
        Note note;
        std::int64_t begin = 0, end = 0;  // in samples of the synth's clock
        double phase = 0;
        std::uint32_t noise = 0x12345u;
        float held = 0;
    };
    static float sample(Voice& voice, std::int64_t at);
    mutable std::mutex mutex_;
    std::vector<Voice> voices_;
    std::int64_t clock_ = 0;
    float volume_ = 0.6f;
};

// Keeps a mix within -1 and 1: linear up to 0.8, then a soft knee.
float limit(float sample);

} // namespace resend::audio
