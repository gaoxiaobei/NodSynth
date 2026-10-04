#include <nodsynth/runtime/PrepareConfig.h>

#include <cmath>

namespace nodsynth::runtime {
const char* prepareErrorText(PrepareError error) noexcept {
    switch (error) {
        case PrepareError::none: return "";
        case PrepareError::invalidSampleRate: return "sample rate must be a finite value from 8000 to 384000";
        case PrepareError::invalidBlockSize: return "block size must be from 1 to 8192 samples";
        case PrepareError::invalidVoiceCount: return "voice count must be from 1 to 16";
        case PrepareError::voiceBudgetMismatch: return "voice count does not match the compiled voice budget";
        case PrepareError::unknownImplementation: return "the graph contains a node with no DSP implementation";
        case PrepareError::budgetExceeded: return "the prepared plan exceeds its byte budget";
        case PrepareError::tooManyParameters: return "a node declares more parameters than the runtime can smooth";
    }
    return "unknown prepare error";
}

bool validSampleRate(double sampleRate) noexcept {
    return std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0;
}

bool validFrameCount(std::uint32_t frames) noexcept { return frames >= 1 && frames <= kMaxFrames; }
} // namespace nodsynth::runtime
