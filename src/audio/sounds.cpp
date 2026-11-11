#include "audio/sounds.hpp"

namespace resend::audio {

Sound hop(std::span<const int> degrees) {
    Sound sound;
    const float volume = degrees.size() > 1 ? 0.30f : 0.40f;
    for (const int degree : degrees) {
        const auto index = static_cast<std::size_t>(((degree % 8) + 8) % 8);
        sound.push_back({Wave::Square, scale[index], 0, 0, 0.09f, volume});
    }
    return sound;
}

Sound ask() {
    return {{Wave::Triangle, 330, 0, 0, 0.07f, 0.55f}, {Wave::Triangle, 440, 0, 0.06f, 0.09f, 0.55f}};
}

Sound answer() {
    return {{Wave::Triangle, 440, 0, 0, 0.07f, 0.55f}, {Wave::Triangle, 330, 0, 0.06f, 0.09f, 0.55f}};
}

Sound thud() {
    return {{Wave::Triangle, 110, 55, 0, 0.16f, 0.9f}, {Wave::Noise, 900, 0, 0, 0.05f, 0.25f}};
}

Sound resolve() {
    return {{Wave::Square, 294, 587, 0, 0.12f, 0.45f}, {Wave::Triangle, 587, 0, 0.11f, 0.22f, 0.6f}};
}

Sound fail() {
    return {{Wave::Square, 196, 147, 0, 0.28f, 0.45f}};
}

Sound alarm() {
    return {{Wave::Square, 880, 0, 0, 0.08f, 0.40f},
            {Wave::Square, 660, 0, 0.09f, 0.08f, 0.40f},
            {Wave::Square, 880, 0, 0.18f, 0.08f, 0.40f},
            {Wave::Square, 660, 0, 0.27f, 0.10f, 0.40f}};
}

Sound goal() {
    return {{Wave::Square, 784, 0, 0, 0.08f, 0.40f}, {Wave::Square, 1047, 0, 0.07f, 0.16f, 0.40f}};
}

Sound win() {
    Sound sound;
    const float arpeggio[] = {523.25f, 659.25f, 783.99f, 1046.5f};
    for (int i = 0; i < 4; ++i)
        sound.push_back({Wave::Square, arpeggio[i], 0, 0.08f * static_cast<float>(i), 0.12f, 0.40f});
    const float chord[] = {523.25f, 659.25f, 783.99f, 1046.5f};
    for (const float frequency : chord) sound.push_back({Wave::Triangle, frequency, 0, 0.34f, 0.60f, 0.45f});
    return sound;
}

Sound click() {
    return {{Wave::Square, 700, 0, 0, 0.03f, 0.18f}};
}

} // namespace resend::audio
