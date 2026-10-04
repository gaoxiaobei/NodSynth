#pragma once

#include <cstdint>
#include <string>

namespace nodsynth::runtime {
inline constexpr std::uint32_t kMaxVoices = 16;
inline constexpr std::uint32_t kMaxFrames = 8192;
inline constexpr std::uint32_t kMaxParametersPerNode = 8;

struct PrepareConfig {
    double sampleRate{48000.0};
    std::uint32_t maxFrames{128};
    std::uint32_t voiceCount{16};
    std::uint64_t maxBytes{32ull << 20};
};

struct EngineConfig {
    PrepareConfig audio{};
    std::uint64_t maxPeakBytes{64ull << 20};
    double crossfadeSeconds{0.015};
    double parameterSmoothSeconds{0.01};
    double stealFadeSeconds{0.01};
    double releaseHoldSeconds{0.5};
};

enum class PrepareError {
    none,
    invalidSampleRate,
    invalidBlockSize,
    invalidVoiceCount,
    voiceBudgetMismatch,
    unknownImplementation,
    budgetExceeded,
    tooManyParameters
};

[[nodiscard]] const char* prepareErrorText(PrepareError error) noexcept;

struct StageResult {
    bool accepted{false};
    const char* reason{""};
};

[[nodiscard]] bool validSampleRate(double sampleRate) noexcept;
[[nodiscard]] bool validFrameCount(std::uint32_t frames) noexcept;
} // namespace nodsynth::runtime
