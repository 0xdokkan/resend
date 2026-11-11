#include "audio/output.hpp"

#if RESEND_HAS_MINIAUDIO
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio.h"
#endif

namespace resend::audio {
namespace {

class NullOutput final : public Output {
public:
    explicit NullOutput(std::string why) : why_(std::move(why)) {}
    bool audible() const override { return false; }
    std::string description() const override { return why_; }

private:
    std::string why_;
};

#if RESEND_HAS_MINIAUDIO
class DeviceOutput final : public Output {
public:
    explicit DeviceOutput(Synth& synth) : synth_(synth) {}
    ~DeviceOutput() override {
        if (open_) ma_device_uninit(&device_);
    }
    bool open() {
        auto config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 1;
        config.sampleRate = sample_rate;
        config.dataCallback = [](ma_device* device, void* out, const void*, ma_uint32 frames) {
            static_cast<DeviceOutput*>(device->pUserData)->synth_.render(static_cast<float*>(out), frames);
        };
        config.pUserData = this;
        if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) return false;
        open_ = true;
        if (ma_device_start(&device_) != MA_SUCCESS) return false;
        name_ = device_.playback.name;
        return true;
    }
    bool audible() const override { return true; }
    std::string description() const override { return name_; }

private:
    Synth& synth_;
    ma_device device_{};
    bool open_ = false;
    std::string name_;
};
#endif

} // namespace

std::unique_ptr<Output> null_output(std::string why) { return std::make_unique<NullOutput>(std::move(why)); }

std::unique_ptr<Output> open_output(Synth& synth) {
#if RESEND_HAS_MINIAUDIO
    auto device = std::make_unique<DeviceOutput>(synth);
    if (device->open()) return device;
    return null_output("no sound device could be opened");
#else
    (void)synth;
    return null_output("built without sound output");
#endif
}

} // namespace resend::audio
