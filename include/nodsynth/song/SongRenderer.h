#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nodsynth/persist/Json.h>
#include <nodsynth/render/OfflineRenderer.h>
#include <nodsynth/song/AudioAnalysis.h>
#include <nodsynth/song/SongDocument.h>

namespace nodsynth::song {
// Equal-power pan. pan -1 is hard left, 0 is center (-3 dB each side), +1 is hard right.
// Stem samples are post gain and pan. With no master effect, the mix is their sum.
struct SongRenderOptions {
    double sampleRate{48000.0};
    std::uint32_t blockSize{128};
    render::TailMode tailMode{render::TailMode::fixed};
    double tailSeconds{2.0};
    double tailThreshold{0.0001};
    double maxTailSeconds{8.0};
    std::uint32_t maxEventsPerBlock{runtime::kMaxBlockMidiEvents};
    midi::RenderMode mode{midi::RenderMode::strict};
    std::filesystem::path baseDirectory;
    std::string songHash;
    struct ExternalTool {
        std::string adapter{"fluidsynth"};
        std::filesystem::path executable;
        std::uint32_t timeoutMs{120000};
    };
    std::vector<ExternalTool> tools;
    std::optional<std::uint32_t> previewStartTick;
    std::optional<std::uint32_t> previewEndTick;
    std::filesystem::path cacheDirectory;
    bool useCache{true};
    std::string quality{"final"};
    bool freezeExternal{false};
};

struct StemReport {
    std::string trackId;
    std::string path;
    std::string hash;
    float peak{0.f};
    std::string adapter{"nodsynth"};
    std::uint32_t latencySamples{0};
};

struct TimingBreakdown {
    double prepareMs{0};
    double prerollMs{0};
    double dspMs{0};
    double externalMs{0};
    double mixMs{0};
    double writeMs{0};
    double analyzeMs{0};
    double totalMs{0};
    std::uint64_t renderedFrames{0};
    std::uint64_t emittedFrames{0};
    double realtimeFactor{0};
};

struct SongRenderReport {
    bool ok{false};
    std::string code;
    std::string message;
    std::uint64_t revision{0};
    std::uint32_t sampleRate{0};
    std::uint32_t blockSize{0};
    std::uint64_t frames{0};
    std::uint64_t originSample{0};
    std::uint64_t tailFrames{0};
    bool tailTruncated{false};
    float peak{0.f};
    std::string mixHash;
    std::string songHash;
    double milliseconds{0};
    TimingBreakdown timing;
    bool cacheHit{false};
    std::string cacheReason{"cache-not-implemented"};
    std::string quality{"final"};
    std::string renderId;
    std::string auditionStatus{"unheard"};
    std::string mixPath;
    std::optional<std::uint32_t> previewStartTick;
    std::optional<std::uint32_t> previewEndTick;
    std::vector<StemReport> stems;
    std::vector<Diagnostic> diagnostics;
};

struct CompareOptions {
    bool matchLoudness{false};
    float silenceThreshold{0.0001f};
};

struct CompareReport {
    bool ok{false};
    std::string message;
    AudioAnalysis a;
    AudioAnalysis b;
    AudioAnalysis matchedB;
    double peakDelta{0};
    double rmsDelta{0};
    std::optional<double> lufsDelta;
    bool loudnessMatched{false};
};

[[nodiscard]] persist::Json songReportJson(const SongRenderReport& report);
[[nodiscard]] SongRenderReport renderSong(
    const SongDocument& song,
    const SongRenderOptions& options,
    const std::filesystem::path& mixOutput,
    const std::filesystem::path& stemsDirectory = {});
[[nodiscard]] CompareReport compareWav(
    const std::filesystem::path& a, const std::filesystem::path& b, const CompareOptions& options = {});
[[nodiscard]] persist::Json compareJson(const CompareReport& report);
} // namespace nodsynth::song
