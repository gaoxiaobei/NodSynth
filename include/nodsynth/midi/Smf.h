#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nodsynth::midi {
inline constexpr std::uint32_t kDefaultTempoUs = 500000;
inline constexpr std::uint32_t kMaxVarlenBytes = 4;

struct Limits {
    std::uint64_t maxFileBytes{32ull << 20};
    std::uint32_t maxTracks{256};
    std::uint32_t maxEvents{1'000'000};
    std::uint32_t maxVarlenBytes{kMaxVarlenBytes};
};

enum class EventKind : std::uint8_t {
    noteOn,
    noteOff,
    pitchBend,
    controlChange,
    programChange,
    channelPressure,
    polyPressure,
    tempo,
    timeSignature,
    sysex,
    meta
};

enum class ParseStatus { ok, rejected };

struct Diagnostic {
    std::string code;
    std::string message;
    std::uint32_t tick{0};
    int track{-1};
};

struct Event {
    std::uint32_t tick{0};
    std::uint16_t track{0};
    std::uint32_t sequence{0};
    std::uint8_t channel{0};
    bool hasChannel{false};
    EventKind kind{EventKind::meta};
    std::uint8_t data1{0};
    std::uint8_t data2{0};
    std::uint32_t tempoUsPerQuarter{0};
    std::uint8_t timeNumerator{4};
    std::uint16_t timeDenominator{4};
    std::uint8_t metaType{0};
    std::vector<std::uint8_t> payload;
};

struct TempoPoint {
    std::uint32_t tick{0};
    std::uint32_t microsecondsPerQuarter{kDefaultTempoUs};
};

struct TimeSignaturePoint {
    std::uint32_t tick{0};
    std::uint8_t numerator{4};
    std::uint16_t denominator{4};
};

struct NoteStream {
    std::uint16_t track{0};
    std::uint8_t channel{0};
    std::uint32_t noteOns{0};
};

struct File {
    std::uint16_t format{0};
    std::uint16_t ppq{480};
    std::uint16_t trackCount{0};
    std::uint32_t endTick{0};
    std::vector<Event> events;
    std::vector<TempoPoint> tempo;
    std::vector<TimeSignaturePoint> timeSignatures;
    std::vector<Diagnostic> diagnostics;
};

struct ParseResult {
    ParseStatus status{ParseStatus::rejected};
    std::string code;
    std::string message;
    File file;
};

struct Selection {
    std::optional<std::uint16_t> track;
    std::optional<std::uint8_t> channel;
};

struct SynthCapabilities {
    bool notes{true};
    bool sustain{true};
    bool allNotesOff{true};
    bool allSoundOff{true};
    bool pitchBend{true};
};

enum class RenderMode { strict, loose };

struct CapabilityReport {
    std::vector<Diagnostic> unsupported;
    std::vector<Diagnostic> preserved;
    bool rejected{false};
};

struct PlaybackEvent {
    std::int64_t absoluteSample{0};
    std::uint64_t blockIndex{0};
    std::uint32_t sampleOffset{0};
    Event source;
};

struct ScheduleResult {
    ParseStatus status{ParseStatus::rejected};
    std::string code;
    std::string message;
    std::vector<PlaybackEvent> events;
};

class TempoMap {
public:
    explicit TempoMap(const File& file);
    [[nodiscard]] std::uint16_t ppq() const noexcept { return ppq_; }
    [[nodiscard]] const std::vector<TempoPoint>& points() const noexcept { return points_; }
    [[nodiscard]] std::optional<std::int64_t> sampleAtTick(std::uint32_t tick, std::uint32_t sampleRate) const;

private:
    struct Segment {
        std::uint32_t tick{0};
        std::uint32_t usPerQuarter{kDefaultTempoUs};
        std::uint64_t numer{0};
    };

    std::uint16_t ppq_{480};
    std::vector<TempoPoint> points_;
    std::vector<Segment> segments_;
};

[[nodiscard]] const char* eventKindName(EventKind kind) noexcept;
[[nodiscard]] ParseResult parse(const std::uint8_t* data, std::size_t size, Limits limits = {});
[[nodiscard]] ParseResult parseFile(const std::filesystem::path& path, Limits limits = {});
[[nodiscard]] std::vector<Event> select(const File& file, const Selection& selection);
[[nodiscard]] std::vector<NoteStream> noteStreams(const File& file);
[[nodiscard]] bool requiresExplicitStream(const File& file);
[[nodiscard]] CapabilityReport assess(const File& file, RenderMode mode, SynthCapabilities capabilities = {});
[[nodiscard]] ScheduleResult schedule(
    const File& file,
    std::uint32_t sampleRate,
    std::uint32_t blockSize,
    const Selection& selection = {});
} // namespace nodsynth::midi
