// Sound (G5): the cues of what the playback shows, the lab's moments and
// the buttons, through the synthesizer of src/audio/. Nothing here changes
// the simulation: sound on, off or with no device, the trace is the same.

#include "app_controller.hpp"

#include "audio/sounds.hpp"
#include "core/text.hpp"

#include <cmath>

namespace resend {
namespace {
using netsim::text::tr;

constexpr std::size_t played_kept = 256;

float level_volume(const std::string& level) {
    if (level == "quiet") return 0.3f;
    if (level == "loud") return 1.0f;
    return 0.6f;
}

audio::Sound sound_of(const Play& play) {
    switch (play.kind) {
    case CueKind::Hop: return audio::hop(play.notes);
    case CueKind::Ask: return audio::ask();
    case CueKind::Answer: return audio::answer();
    case CueKind::Thud: return audio::thud();
    case CueKind::Resolve: return audio::resolve();
    case CueKind::Fail: return audio::fail();
    case CueKind::Alarm: return audio::alarm();
    case CueKind::Goal: return audio::goal();
    case CueKind::Win: return audio::win();
    case CueKind::Click: return audio::click();
    }
    return {};
}

} // namespace

void AppController::bind_sound() {
    const auto& sound = ui_->global<UiSound>();
    sound.on_set_sound([this](const slint::SharedString& value) {
        const std::string text(value);
        if (text == "on" || text == "off" || text == "next") {
            set_sound(text == "next" ? !settings_.sound : text == "on");
        } else if (text == "quiet" || text == "medium" || text == "loud") {
            set_volume(text);
        }
    });
    sound.on_click([this] {
        // A click is real time, not the playback's: two within the merge
        // window are one.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_click_ < CueMixer::merge_window) return;
        last_click_ = now;
        play(CueKind::Click);
    });
    sound.set_device(slint::SharedString(output_->description()));
}

void AppController::apply_sound() {
    synth_.set_volume(level_volume(settings_.volume));
    ui_->global<UiSound>().set_on(settings_.sound);
    ui_->global<UiSound>().set_level(slint::SharedString(settings_.volume));
}

void AppController::set_sound(bool on) {
    settings_.sound = on;
    save_settings();
    if (!on) synth_.stop();
    ui_->global<UiSound>().set_on(on);
    refresh_commands();
    status(on ? tr("Sound on: what the canvas shows can be heard too.") : tr("Sound off."));
    if (on) play(CueKind::Click);
}

void AppController::set_volume(const std::string& level) {
    settings_.volume = level;
    save_settings();
    synth_.set_volume(level_volume(level));
    ui_->global<UiSound>().set_level(slint::SharedString(level));
    refresh_commands();
    if (!settings_.sound) set_sound(true);
    else play(CueKind::Click);
}

void AppController::play(const std::vector<Play>& plays) {
    if (!settings_.sound) return;
    // Faster playback, shorter notes, so that they keep up.
    const double speed = playback_.speed();
    const float stretch = speed > 1 ? static_cast<float>(1 / std::sqrt(speed)) : 1.0f;
    for (const auto& one : plays) {
        if (output_->audible()) synth_.play(sound_of(one), stretch);
        played_.push_back(one);
        ++played_count_;
    }
    if (played_.size() > played_kept)
        played_.erase(played_.begin(), played_.begin() + static_cast<std::ptrdiff_t>(played_.size() - played_kept));
}

void AppController::play(CueKind kind) { play(std::vector<Play>{{kind, {}}}); }

} // namespace resend
