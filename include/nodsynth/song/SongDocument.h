#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nodsynth/midi/Smf.h>
#include <nodsynth/persist/Json.h>

namespace nodsynth::song {
inline constexpr int kSongFormatVersion = 2;
inline constexpr int kMinSupportedSongFormatVersion = 1;

struct Diagnostic {
    std::string code;
    std::string message;
    std::uint32_t tick{0};
    int track{-1};
    std::string objectId;
};

struct TempoPoint {
    std::uint32_t tick{0};
    std::uint32_t microsecondsPerQuarter{midi::kDefaultTempoUs};
};

struct TimeSignaturePoint {
    std::uint32_t tick{0};
    std::uint8_t numerator{4};
    std::uint16_t denominator{4};
};

struct Resource {
    std::string id;
    std::string path;
    std::string hash;
    std::string kind{"patch"};
};

enum class InstrumentKind { nodsynth, externalCli, vst3 };

struct Instrument {
    std::string id;
    InstrumentKind kind{InstrumentKind::nodsynth};
    std::string resourceId;
    std::string name;
    std::string adapter;
};

struct Note {
    std::string id;
    std::uint32_t tick{0};
    std::uint32_t duration{0};
    std::uint8_t pitch{60};
    std::uint8_t velocity{100};
    std::uint8_t channel{0};
};

struct PerformanceEvent {
    std::string id;
    std::uint32_t tick{0};
    midi::EventKind kind{midi::EventKind::controlChange};
    std::uint8_t channel{0};
    std::uint8_t data1{0};
    std::uint8_t data2{0};
};

struct Clip {
    std::string id;
    std::uint32_t startTick{0};
    std::uint32_t length{0};
    std::vector<Note> notes;
};

struct GainPoint {
    std::uint32_t tick{0};
    double gain{1.0};
};

struct ParameterPoint {
    std::uint32_t tick{0};
    double value{0};
};

struct ParameterLane {
    std::string id;
    std::vector<ParameterPoint> points;
};

struct Track {
    std::string id;
    std::string name;
    std::string instrumentId;
    double gain{1.0};
    double pan{0.0};
    int order{0};
    std::optional<std::uint16_t> sourceTrack;
    std::optional<std::uint8_t> sourceChannel;
    std::vector<Clip> clips;
    std::vector<GainPoint> gainAutomation;
    std::vector<ParameterLane> parameterAutomation;
    std::vector<PerformanceEvent> performance;
};

struct PreservedEvent {
    std::uint32_t tick{0};
    int sourceTrack{-1};
    std::uint32_t sequence{0};
    midi::EventKind kind{midi::EventKind::meta};
    bool hasChannel{false};
    std::uint8_t channel{0};
    std::uint8_t data1{0};
    std::uint8_t data2{0};
    std::uint8_t metaType{0};
    bool affectsSound{false};
    std::vector<std::uint8_t> payload;
};

struct SongDocument {
    int version{kSongFormatVersion};
    std::uint64_t revision{1};
    std::uint16_t ppq{480};
    std::optional<std::uint32_t> songRangeEndTick;
    std::vector<TempoPoint> tempo;
    std::vector<TimeSignaturePoint> timeSignatures;
    std::vector<Resource> resources;
    std::vector<Instrument> instruments;
    std::vector<Track> tracks;
    std::vector<PreservedEvent> preserved;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> appliedRequests;
    std::vector<std::string> undoStack;
    std::vector<std::string> redoStack;
    std::string sourceMidiHash;
};

struct Validation {
    bool ok{false};
    std::vector<Diagnostic> diagnostics;
};

struct ApplyResult {
    bool ok{false};
    bool unchanged{false};
    bool noop{false};
    std::string code;
    std::string message;
    std::uint64_t baseRevision{0};
    std::uint64_t revision{0};
    std::string requestId;
    persist::Json diff{persist::Json::object()};
    persist::Json idMap{persist::Json::object()};
};

struct QueryOptions {
    std::string view{"legacy"};
    std::optional<std::string> trackId;
    std::optional<std::uint32_t> startTick;
    std::optional<std::uint32_t> endTick;
    std::optional<std::uint32_t> limit;
    std::optional<std::string> cursor;
    bool onsetOnly{false};
    bool expandAutomation{false};
};

struct CreateSongOptions {
    double bpm{120.0};
    std::uint8_t numerator{4};
    std::uint16_t denominator{4};
    std::uint16_t ppq{480};
    std::uint32_t bars{16};
};

struct StreamMap {
    std::uint16_t sourceTrack{0};
    std::uint8_t sourceChannel{0};
    std::string patchPath;
    std::string name;
};

struct ImportOptions {
    std::vector<StreamMap> maps;
};

struct ImportResult {
    bool ok{false};
    std::string code;
    std::string message;
    SongDocument song;
};

[[nodiscard]] std::string hashFile(const std::filesystem::path& path);
[[nodiscard]] persist::Json toJson(const SongDocument& song);
[[nodiscard]] std::optional<SongDocument> songFromJson(const persist::Json& json, std::string& error);
[[nodiscard]] std::optional<SongDocument> loadSong(const std::filesystem::path& path, std::string& error);
[[nodiscard]] bool saveSong(const std::filesystem::path& path, const SongDocument& song, std::string& error);
[[nodiscard]] Validation validate(const SongDocument& song);
[[nodiscard]] Validation validateDocument(const SongDocument& song);
[[nodiscard]] Validation validateRenderReady(const SongDocument& song);
[[nodiscard]] persist::Json summaryJson(const SongDocument& song, const Validation& validation);
[[nodiscard]] SongDocument createSong(const CreateSongOptions& options);
[[nodiscard]] persist::Json querySong(
    const SongDocument& song, std::optional<std::uint32_t> startTick = std::nullopt, std::optional<std::uint32_t> endTick = std::nullopt);
[[nodiscard]] persist::Json querySong(const SongDocument& song, const QueryOptions& options);
[[nodiscard]] ApplyResult applyCommands(
    SongDocument& song,
    const persist::Json& batch,
    std::optional<std::uint64_t> expectRevision = std::nullopt,
    bool dryRun = false);
[[nodiscard]] ApplyResult undoSong(SongDocument& song);
[[nodiscard]] ApplyResult redoSong(SongDocument& song);
[[nodiscard]] persist::Json semanticDiff(const SongDocument& before, const SongDocument& after, bool expandAutomation = false);
[[nodiscard]] bool bindPatch(
    SongDocument& song,
    const std::string& trackId,
    std::string storedPath,
    const std::filesystem::path& fileToHash,
    std::string& error);
[[nodiscard]] bool bindExternal(
    SongDocument& song,
    const std::string& trackId,
    std::string adapter,
    std::string storedPath,
    const std::filesystem::path& fileToHash,
    std::string& error);

[[nodiscard]] ImportResult importMidi(const midi::File& file, const ImportOptions& options = {});
[[nodiscard]] std::vector<std::uint8_t> exportMidiBytes(const SongDocument& song, std::string& error);
[[nodiscard]] bool exportMidi(const SongDocument& song, const std::filesystem::path& path, std::string& error);
[[nodiscard]] std::vector<std::uint8_t> exportTrackMidi(const SongDocument& song, const std::string& trackId, std::string& error);
[[nodiscard]] std::optional<std::int64_t> sampleAtTick(const SongDocument& song, std::uint32_t tick, std::uint32_t sampleRate);
[[nodiscard]] std::uint32_t endTick(const SongDocument& song);
[[nodiscard]] bool barsToTicks(
    const SongDocument& song, std::uint32_t startBar, std::uint32_t endBar, std::uint32_t& startTick, std::uint32_t& endTick, std::string& error);
} // namespace nodsynth::song
