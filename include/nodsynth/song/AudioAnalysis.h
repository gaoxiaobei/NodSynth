#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nodsynth/persist/Json.h>

namespace nodsynth::song {
struct AnalyzeOptions {
    double windowMs{10.0};
    double thresholdDb{-60.0};
    double minSilenceMs{100.0};
};

struct AudioAnalysis {
    bool ok{false};
    std::string message;
    std::uint32_t sampleRate{0};
    std::uint32_t channels{0};
    std::uint64_t frames{0};
    float peak{0.f};
    std::optional<double> loudnessLufs;
    std::optional<double> truePeakDbtp,shortTermMaxLufs,loudnessRangeLu;
    std::string meterVersion{"libebur128-1.2.6"};
    std::uint64_t silentFrames{0};
    // Compatibility: true if any sample at the start (or the whole file) is at or
    // below the sample-peak silenceThreshold. This is not a musical silence gate.
    bool leadingSilence{false};
    // Compatibility: true if the last sample-peak is at or below silenceThreshold.
    bool trailingSilence{false};
    double leadingSilenceSeconds{0.0};
    double trailingSilenceSeconds{0.0};
    std::optional<double> activityStartSeconds;
    std::optional<double> activityEndSeconds;
    double activeRatio{0.0};
    double windowSeconds{0.0};
    double thresholdAmplitude{0.0};
    std::optional<double> samplePeakDb;
    double rms{0.0};
    bool fullySilent{false};
    std::optional<double> correlation;
    double midEnergy{0}, sideEnergy{0}, monoRms{0}, dcLeft{0}, dcRight{0};
};

[[nodiscard]] AudioAnalysis analyzeWav(const std::filesystem::path& path, float silenceThreshold = 0.0001f);
[[nodiscard]] AudioAnalysis analyzeWav(const std::filesystem::path& path, const AnalyzeOptions& options);
[[nodiscard]] persist::Json analysisJson(const AudioAnalysis& analysis);
} // namespace nodsynth::song
