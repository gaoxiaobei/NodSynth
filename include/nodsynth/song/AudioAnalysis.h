#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nodsynth/persist/Json.h>

namespace nodsynth::song {
struct AudioAnalysis {
    bool ok{false};
    std::string message;
    std::uint32_t sampleRate{0};
    std::uint32_t channels{0};
    std::uint64_t frames{0};
    float peak{0.f};
    std::optional<double> loudnessLufs;
    std::uint64_t silentFrames{0};
    bool leadingSilence{false};
    bool trailingSilence{false};
};

[[nodiscard]] AudioAnalysis analyzeWav(const std::filesystem::path& path, float silenceThreshold = 0.0001f);
[[nodiscard]] persist::Json analysisJson(const AudioAnalysis& analysis);
} // namespace nodsynth::song
