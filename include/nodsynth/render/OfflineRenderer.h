#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nodsynth/midi/Smf.h>
#include <nodsynth/model/GraphDocument.h>
#include <nodsynth/persist/Json.h>
#include <nodsynth/runtime/VoiceAllocator.h>

namespace nodsynth::render {
enum class TailMode { fixed, threshold };

struct RenderOptions {
    double sampleRate{48000.0};
    std::uint32_t blockSize{128};
    midi::Selection selection{};
    bool mergeChannels{false};
    midi::RenderMode mode{midi::RenderMode::strict};
    TailMode tailMode{TailMode::fixed};
    double tailSeconds{2.0};
    double tailThreshold{0.0001};
    double maxTailSeconds{8.0};
    std::uint32_t maxEventsPerBlock{runtime::kMaxBlockMidiEvents};
    std::string midiHash;
    std::string patchHash;
};

struct RenderReport {
    bool ok{false};
    std::string code;
    std::string message;
    std::uint32_t sampleRate{0};
    std::uint32_t blockSize{0};
    std::uint64_t frames{0};
    std::uint64_t tailFrames{0};
    bool tailTruncated{false};
    float peak{0.f};
    std::uint32_t activeVoicesAtEnd{0};
    std::string outputHash;
    std::string midiHash;
    std::string patchHash;
    double milliseconds{0};
    std::vector<midi::Diagnostic> diagnostics;
};

[[nodiscard]] persist::Json inspectMidi(const midi::File& file);
[[nodiscard]] persist::Json reportJson(const RenderReport& report, const midi::File* file = nullptr);
[[nodiscard]] std::string hashFile(const std::filesystem::path& path);
[[nodiscard]] std::optional<model::GraphSnapshot> loadPatch(const std::filesystem::path& path, std::string& error);
[[nodiscard]] RenderReport renderMidi(
    const model::GraphSnapshot& graph,
    const midi::File& file,
    const RenderOptions& options,
    const std::filesystem::path& output);
} // namespace nodsynth::render
