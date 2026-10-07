#pragma once

#include <array>
#include <span>
#include <nodsynth/runtime/VoiceAllocator.h>
#include <nodsynth/song/SongDocument.h>

namespace nodsynth::song {
[[nodiscard]] persist::Json sampleLayersJson(const std::vector<SampleLayer>& layers);
[[nodiscard]] bool parseSampleLayers(const persist::Json& value, std::vector<SampleLayer>& layers, std::string& error);
[[nodiscard]] bool validateSampleLayers(const Instrument& instrument, const SongDocument& song, std::string& error);
[[nodiscard]] bool createSampleKit(const std::filesystem::path& directory, std::string& error, bool punchKick = false);
[[nodiscard]] bool bindSampleKit(SongDocument& song, const std::string& trackId, const std::filesystem::path& manifest, std::string& error);

// File access and bandlimited resampling happen only in prepare. Playback owns 16 voices.
class OneShotSampler {
public:
    [[nodiscard]] bool prepare(const Instrument& instrument, const SongDocument& song,
        const std::filesystem::path& base, std::uint32_t sampleRate, std::string& error,
        std::uint64_t maxBytes = 256ull * 1024 * 1024);
    void reset() noexcept;
    void process(float* left, float* right, std::uint32_t frames, std::span<const runtime::MidiEvent> events) noexcept;
    [[nodiscard]] std::uint64_t committedBytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint64_t stolenNotes() const noexcept { return stolen_; }
private:
    struct Layer {
        SampleLayer config;
        std::array<std::vector<float>, 128> audio;
    };
    struct Voice {
        const std::vector<float>* audio{nullptr};
        std::size_t position{0};
        std::uint64_t age{0};
        int channel{0}, choke{0};
        float gain{1}, lastLeft{0}, lastRight{0};
        float residualLeft{0}, residualRight{0};
        std::uint32_t fadeLeft{0}, stopLeft{0};
    };
    void apply(const runtime::MidiEvent& event) noexcept;
    std::vector<Layer> layers_;
    std::vector<std::array<std::uint64_t, 16>> rotations_;
    std::array<Voice, 16> voices_{};
    std::uint32_t fadeFrames_{96};
    std::uint64_t bytes_{0}, age_{0}, stolen_{0};
};
}
