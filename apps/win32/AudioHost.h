#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace nodsynth::app {
struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

struct MidiDevice {
    unsigned index{0};
    std::wstring name;
};

using RenderCallback = void (*)(void* context, float* const* channels, std::uint32_t channelCount, std::uint32_t frames);

[[nodiscard]] std::vector<AudioDevice> renderDevices();
[[nodiscard]] std::vector<MidiDevice> midiDevices();

class WasapiOutput {
public:
    WasapiOutput();
    ~WasapiOutput();

    bool start(const std::wstring& deviceId, RenderCallback callback, void* context, std::string& error);
    void begin();
    void stop();
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_.load(); }
    [[nodiscard]] std::uint32_t bufferFrames() const noexcept { return bufferFrames_.load(); }

private:
    void threadMain();
    bool open(const std::wstring& deviceId, std::string& error);
    void close() noexcept;
    void renderAvailable();

    RenderCallback callback_{nullptr};
    void* context_{nullptr};
    std::thread thread_;
    std::wstring requestedId_;
    std::string error_;
    std::atomic<int> command_{0};
    std::atomic<double> sampleRate_{0};
    std::atomic<std::uint32_t> bufferFrames_{0};
    void* client_{nullptr};
    void* render_{nullptr};
    void* event_{nullptr};
    void* wake_{nullptr};
    std::vector<float> left_;
    std::vector<float> right_;
};

class MidiIn {
public:
    using Callback = void (*)(void* context, std::uint8_t status, std::uint8_t data1, std::uint8_t data2);
    bool open(unsigned index, Callback callback, void* context, std::string& error);
    void close();
    void dispatch(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept;

private:
    void* handle_{nullptr};
    Callback callback_{nullptr};
    void* context_{nullptr};
};
} // namespace nodsynth::app
