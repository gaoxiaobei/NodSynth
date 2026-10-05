#include <nodsynth/midi/Smf.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

namespace nodsynth::midi {
namespace {
struct Wide {
    std::uint64_t hi{0};
    std::uint64_t lo{0};
};

Wide mul64(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a0 = a & 0xffffffffull;
    const std::uint64_t a1 = a >> 32;
    const std::uint64_t b0 = b & 0xffffffffull;
    const std::uint64_t b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0;
    const std::uint64_t p01 = a0 * b1;
    const std::uint64_t p10 = a1 * b0;
    const std::uint64_t p11 = a1 * b1;
    const std::uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffull) + (p10 & 0xffffffffull);
    Wide result;
    result.lo = (p00 & 0xffffffffull) | (mid << 32);
    result.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return result;
}

std::optional<std::uint64_t> divRound(Wide value, std::uint64_t divisor) {
    if (divisor == 0) return std::nullopt;
    const std::uint64_t half = divisor >> 1;
    const auto previous = value.lo;
    value.lo += half;
    if (value.lo < previous) ++value.hi;
    if (value.hi >= divisor) return std::nullopt;
    std::uint64_t remainder = value.hi;
    std::uint64_t quotient = 0;
    for (int bit = 63; bit >= 0; --bit) {
        if (remainder > (std::numeric_limits<std::uint64_t>::max() >> 1)) return std::nullopt;
        const auto next = (value.lo >> static_cast<unsigned>(bit)) & 1ull;
        remainder = (remainder << 1) | next;
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient |= 1ull << static_cast<unsigned>(bit);
        }
    }
    return quotient;
}

ParseResult reject(std::string code, std::string message) {
    ParseResult result;
    result.code = std::move(code);
    result.message = std::move(message);
    return result;
}

class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    [[nodiscard]] std::size_t index() const noexcept { return index_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool exhausted() const noexcept { return index_ >= size_; }

    void setLimit(std::size_t limit) { limit_ = limit; }
    void clearLimit() { limit_ = size_; }
    [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

    bool seek(std::size_t index, std::string& error) {
        if (index > limit_) {
            error = "truncated";
            return false;
        }
        index_ = index;
        return true;
    }

    bool u8(std::uint8_t& value, std::string& error) {
        if (index_ >= limit_) {
            error = "truncated";
            return false;
        }
        value = data_[index_++];
        return true;
    }

    bool u16(std::uint16_t& value, std::string& error) {
        std::uint8_t hi = 0;
        std::uint8_t lo = 0;
        if (!u8(hi, error) || !u8(lo, error)) return false;
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(hi) << 8) | lo);
        return true;
    }

    bool u32(std::uint32_t& value, std::string& error) {
        std::uint8_t bytes[4]{};
        for (auto& byte : bytes) {
            if (!u8(byte, error)) return false;
        }
        value = (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
                (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
        return true;
    }

    bool bytes(std::size_t count, std::vector<std::uint8_t>& out, std::string& error) {
        if (count > limit_ - index_) {
            error = "truncated";
            return false;
        }
        out.insert(out.end(), data_ + index_, data_ + index_ + count);
        index_ += count;
        return true;
    }

    bool varlen(std::uint32_t& value, std::uint32_t maxBytes, std::string& error) {
        value = 0;
        const auto capped = std::min(maxBytes, kMaxVarlenBytes);
        for (std::uint32_t used = 0; used < kMaxVarlenBytes; ++used) {
            std::uint8_t byte = 0;
            if (!u8(byte, error)) return false;
            if (used >= capped) {
                error = "bad-varlen";
                return false;
            }
            if (value > (std::numeric_limits<std::uint32_t>::max() >> 7)) {
                error = "bad-varlen";
                return false;
            }
            value = (value << 7) | static_cast<std::uint32_t>(byte & 0x7f);
            if ((byte & 0x80) == 0) return true;
        }
        error = "bad-varlen";
        return false;
    }

private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
    std::size_t index_{0};
    std::size_t limit_{0};
};

bool tag(Cursor& cursor, const char* text, std::string& error) {
    std::uint8_t bytes[4]{};
    for (auto& byte : bytes) {
        if (!cursor.u8(byte, error)) return false;
    }
    if (std::memcmp(bytes, text, 4) != 0) {
        error = "bad-header";
        return false;
    }
    return true;
}

const char* metaName(std::uint8_t type) {
    switch (type) {
    case 0x01: return "text";
    case 0x02: return "copyright";
    case 0x03: return "track-name";
    case 0x04: return "instrument";
    case 0x05: return "lyric";
    case 0x06: return "marker";
    case 0x07: return "cue";
    default: return "meta";
    }
}

bool channelEvent(Event& event, std::uint8_t status, std::uint8_t first, bool hasFirst, Cursor& cursor, std::string& error) {
    const auto opcode = static_cast<std::uint8_t>(status & 0xf0);
    const auto dataBytes = (opcode == 0xc0 || opcode == 0xd0) ? 1 : 2;
    std::uint8_t data[2]{};
    int filled = 0;
    if (hasFirst) data[filled++] = first;
    while (filled < dataBytes) {
        if (!cursor.u8(data[filled], error)) return false;
        if ((data[filled] & 0x80) != 0) {
            error = "bad-event";
            return false;
        }
        ++filled;
    }
    event.hasChannel = true;
    event.channel = static_cast<std::uint8_t>(status & 0x0f);
    event.data1 = data[0];
    if (dataBytes == 2) event.data2 = data[1];
    switch (opcode) {
    case 0x80:
        event.kind = EventKind::noteOff;
        break;
    case 0x90:
        if (event.data2 == 0) event.kind = EventKind::noteOff;
        else event.kind = EventKind::noteOn;
        break;
    case 0xa0:
        event.kind = EventKind::polyPressure;
        break;
    case 0xb0:
        event.kind = EventKind::controlChange;
        break;
    case 0xc0:
        event.kind = EventKind::programChange;
        break;
    case 0xd0:
        event.kind = EventKind::channelPressure;
        break;
    case 0xe0:
        event.kind = EventKind::pitchBend;
        break;
    default:
        error = "bad-event";
        return false;
    }
    return true;
}

void resolveTempo(File& file) {
    struct Item {
        std::uint32_t tick;
        std::uint16_t track;
        std::uint32_t sequence;
        std::uint32_t us;
    };
    std::vector<Item> items;
    for (const auto& event : file.events) {
        if (event.kind == EventKind::tempo) items.push_back({event.tick, event.track, event.sequence, event.tempoUsPerQuarter});
    }
    std::vector<TempoPoint> points;
    for (std::size_t index = 0; index < items.size();) {
        const auto tick = items[index].tick;
        std::size_t end = index + 1;
        while (end < items.size() && items[end].tick == tick) ++end;
        Item winner = items[index];
        for (std::size_t cursor = index + 1; cursor < end; ++cursor) {
            const auto& item = items[cursor];
            if (item.track < winner.track || (item.track == winner.track && item.sequence > winner.sequence)) winner = item;
        }
        for (std::size_t cursor = index; cursor < end; ++cursor) {
            const auto& item = items[cursor];
            const auto same = item.track == winner.track && item.sequence == winner.sequence;
            if (!same && item.us != winner.us) {
                file.diagnostics.push_back(
                    {"tempo-conflict",
                     "tempo " + std::to_string(item.us) + " at tick " + std::to_string(item.tick) + " track " +
                         std::to_string(item.track) + " conflicts with track " + std::to_string(winner.track),
                     item.tick, item.track});
            }
        }
        points.push_back({tick, winner.us});
        index = end;
    }
    if (points.empty() || points.front().tick != 0) points.insert(points.begin(), {0, kDefaultTempoUs});
    file.tempo = std::move(points);
}

void resolveTimeSignatures(File& file) {
    struct Item {
        std::uint32_t tick;
        std::uint16_t track;
        std::uint32_t sequence;
        std::uint8_t numerator;
        std::uint16_t denominator;
    };
    std::vector<Item> items;
    for (const auto& event : file.events) {
        if (event.kind == EventKind::timeSignature) {
            items.push_back({event.tick, event.track, event.sequence, event.timeNumerator, event.timeDenominator});
        }
    }
    std::vector<TimeSignaturePoint> points;
    for (std::size_t index = 0; index < items.size();) {
        const auto tick = items[index].tick;
        std::size_t end = index + 1;
        while (end < items.size() && items[end].tick == tick) ++end;
        Item winner = items[index];
        for (std::size_t cursor = index + 1; cursor < end; ++cursor) {
            const auto& item = items[cursor];
            if (item.track < winner.track || (item.track == winner.track && item.sequence > winner.sequence)) winner = item;
        }
        for (std::size_t cursor = index; cursor < end; ++cursor) {
            const auto& item = items[cursor];
            const auto same = item.track == winner.track && item.sequence == winner.sequence;
            const auto sameValue = item.numerator == winner.numerator && item.denominator == winner.denominator;
            if (!same && !sameValue) {
                file.diagnostics.push_back(
                    {"time-signature-conflict",
                     "time signature at tick " + std::to_string(item.tick) + " track " + std::to_string(item.track) +
                         " conflicts with track " + std::to_string(winner.track),
                     item.tick, item.track});
            }
        }
        points.push_back({tick, winner.numerator, winner.denominator});
        index = end;
    }
    if (points.empty() || points.front().tick != 0) points.insert(points.begin(), {0, 4, 4});
    file.timeSignatures = std::move(points);
}

bool supportedControl(std::uint8_t controller, const SynthCapabilities& capabilities) {
    if (controller == 64) return capabilities.sustain;
    if (controller == 120) return capabilities.allSoundOff;
    if (controller == 123) return capabilities.allNotesOff;
    return false;
}

bool isSupported(const Event& event, const SynthCapabilities& capabilities) {
    switch (event.kind) {
    case EventKind::noteOn:
    case EventKind::noteOff:
        return capabilities.notes;
    case EventKind::pitchBend:
        return capabilities.pitchBend;
    case EventKind::controlChange:
        return supportedControl(event.data1, capabilities);
    case EventKind::tempo:
    case EventKind::timeSignature:
    case EventKind::meta:
        return true;
    case EventKind::programChange:
    case EventKind::channelPressure:
    case EventKind::polyPressure:
    case EventKind::sysex:
        return false;
    }
    return false;
}

bool affectsSound(const Event& event) {
    return event.kind != EventKind::tempo && event.kind != EventKind::timeSignature && event.kind != EventKind::meta;
}

std::string describe(const Event& event) {
    std::string text = eventKindName(event.kind);
    if (event.kind == EventKind::controlChange) text += " " + std::to_string(event.data1);
    else if (event.kind == EventKind::programChange) text += " " + std::to_string(event.data1);
    else if (event.kind == EventKind::meta) text = std::string(metaName(event.metaType)) + " " + std::to_string(event.metaType);
    text += " at tick " + std::to_string(event.tick) + " track " + std::to_string(event.track);
    return text;
}

void normalizeRetriggers(std::vector<Event>& events) {
    for (std::size_t start = 0; start < events.size();) {
        std::size_t end = start + 1;
        while (end < events.size() && events[end].tick == events[start].tick && events[end].track == events[start].track) ++end;
        struct Key {
            std::uint8_t channel;
            std::uint8_t note;
            bool operator==(const Key& other) const noexcept { return channel == other.channel && note == other.note; }
        };
        std::vector<std::pair<Key, std::vector<std::size_t>>> groups;
        for (std::size_t index = start; index < end; ++index) {
            const auto& event = events[index];
            if (!event.hasChannel || (event.kind != EventKind::noteOn && event.kind != EventKind::noteOff)) continue;
            const Key key{event.channel, event.data1};
            auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) { return group.first == key; });
            if (found == groups.end()) groups.push_back({key, {index}});
            else found->second.push_back(index);
        }
        for (const auto& group : groups) {
            std::vector<Event> notes;
            notes.reserve(group.second.size());
            for (const auto index : group.second) notes.push_back(events[index]);
            std::stable_partition(notes.begin(), notes.end(), [](const Event& event) { return event.kind == EventKind::noteOff; });
            for (std::size_t slot = 0; slot < group.second.size(); ++slot) events[group.second[slot]] = std::move(notes[slot]);
        }
        start = end;
    }
}
} // namespace

const char* eventKindName(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::noteOn: return "note-on";
    case EventKind::noteOff: return "note-off";
    case EventKind::pitchBend: return "pitch-bend";
    case EventKind::controlChange: return "control-change";
    case EventKind::programChange: return "program-change";
    case EventKind::channelPressure: return "channel-pressure";
    case EventKind::polyPressure: return "poly-pressure";
    case EventKind::tempo: return "tempo";
    case EventKind::timeSignature: return "time-signature";
    case EventKind::sysex: return "sysex";
    case EventKind::meta: return "meta";
    }
    return "unknown";
}

ParseResult parse(const std::uint8_t* data, std::size_t size, Limits limits) {
    if (data == nullptr && size != 0) return reject("truncated", "missing MIDI data");
    if (size > limits.maxFileBytes) return reject("file-too-large", "MIDI file exceeds the size limit");
    if (limits.maxVarlenBytes == 0 || limits.maxVarlenBytes > kMaxVarlenBytes) {
        return reject("bad-varlen", "variable-length limit must be from 1 to 4 bytes");
    }
    Cursor cursor(data, size);
    cursor.clearLimit();
    std::string error;
    if (!tag(cursor, "MThd", error)) return reject(error == "truncated" ? "truncated" : "bad-header", "missing MThd");
    std::uint32_t headerLength = 0;
    std::uint16_t format = 0;
    std::uint16_t trackCount = 0;
    std::uint16_t division = 0;
    if (!cursor.u32(headerLength, error) || !cursor.u16(format, error) || !cursor.u16(trackCount, error) || !cursor.u16(division, error)) {
        return reject("truncated", "truncated MIDI header");
    }
    if (headerLength < 6) return reject("bad-header", "MIDI header is shorter than 6 bytes");
    const auto headerEnd = 8 + static_cast<std::size_t>(headerLength);
    if (!cursor.seek(headerEnd, error)) return reject("truncated", "truncated MIDI header");
    if (format > 1) return reject("unsupported-format", "only SMF type 0 and type 1 are supported");
    if (trackCount == 0) return reject("bad-header", "MIDI file has no tracks");
    if (trackCount > limits.maxTracks) return reject("too-many-tracks", "MIDI track count exceeds the limit");
    if ((division & 0x8000) != 0) return reject("smpte-division", "SMPTE division is not supported");
    if (division == 0) return reject("bad-header", "PPQ division must be positive");
    if (format == 0 && trackCount != 1) return reject("type0-track-count", "SMF type 0 must contain one track");

    File file;
    file.format = format;
    file.ppq = division;
    file.trackCount = trackCount;
    std::uint32_t eventCount = 0;
    for (std::uint16_t track = 0; track < trackCount; ++track) {
        cursor.clearLimit();
        if (!tag(cursor, "MTrk", error)) {
            if (error == "truncated") return reject("truncated", "truncated MIDI track");
            return reject("bad-track", "expected MTrk");
        }
        std::uint32_t chunkLength = 0;
        if (!cursor.u32(chunkLength, error)) return reject("truncated", "truncated MIDI track");
        const auto chunkEnd = cursor.index() + static_cast<std::size_t>(chunkLength);
        if (chunkEnd < cursor.index() || chunkEnd > cursor.size()) return reject("truncated", "truncated MIDI track");
        cursor.setLimit(chunkEnd);
        std::uint32_t tick = 0;
        std::uint32_t sequence = 0;
        std::uint8_t running = 0;
        bool ended = false;
        while (cursor.index() < chunkEnd && !ended) {
            std::uint32_t delta = 0;
            if (!cursor.varlen(delta, limits.maxVarlenBytes, error)) {
                return reject(error == "bad-varlen" ? "bad-varlen" : "truncated", error == "bad-varlen" ? "variable-length quantity is invalid" : "truncated MIDI event");
            }
            if (delta > std::numeric_limits<std::uint32_t>::max() - tick) return reject("tick-overflow", "MIDI tick counter overflowed");
            tick += delta;
            if (eventCount >= limits.maxEvents) return reject("too-many-events", "MIDI event count exceeds the limit");
            std::uint8_t status = 0;
            if (!cursor.u8(status, error)) return reject("truncated", "truncated MIDI event");
            Event event;
            event.tick = tick;
            event.track = track;
            event.sequence = sequence++;
            ++eventCount;
            if (status == 0xff) {
                running = 0;
                std::uint8_t type = 0;
                std::uint32_t length = 0;
                if (!cursor.u8(type, error) || !cursor.varlen(length, limits.maxVarlenBytes, error)) {
                    return reject(error == "bad-varlen" ? "bad-varlen" : "truncated", "truncated meta event");
                }
                if (!cursor.bytes(length, event.payload, error)) return reject("truncated", "truncated meta event");
                if (type == 0x2f) {
                    ended = true;
                    file.endTick = std::max(file.endTick, tick);
                    continue;
                }
                event.kind = EventKind::meta;
                event.metaType = type;
                if (type == 0x51) {
                    if (length != 3) return reject("bad-tempo", "tempo meta event must be 3 bytes");
                    event.kind = EventKind::tempo;
                    event.tempoUsPerQuarter = (static_cast<std::uint32_t>(event.payload[0]) << 16) |
                                              (static_cast<std::uint32_t>(event.payload[1]) << 8) | event.payload[2];
                    if (event.tempoUsPerQuarter == 0) return reject("bad-tempo", "tempo must be positive");
                } else if (type == 0x58) {
                    if (length < 4) return reject("bad-time-signature", "time signature meta event must be 4 bytes");
                    if (event.payload[1] > 15) return reject("bad-time-signature", "time signature denominator is invalid");
                    event.kind = EventKind::timeSignature;
                    event.timeNumerator = event.payload[0];
                    event.timeDenominator = static_cast<std::uint16_t>(1u << event.payload[1]);
                }
                file.events.push_back(std::move(event));
            } else if (status == 0xf0 || status == 0xf7) {
                running = 0;
                std::uint32_t length = 0;
                if (!cursor.varlen(length, limits.maxVarlenBytes, error)) {
                    return reject(error == "bad-varlen" ? "bad-varlen" : "truncated", "truncated sysex event");
                }
                event.kind = EventKind::sysex;
                event.data1 = status;
                if (!cursor.bytes(length, event.payload, error)) return reject("truncated", "truncated sysex event");
                file.events.push_back(std::move(event));
            } else if ((status & 0x80) == 0) {
                if (running == 0) return reject("running-status-missing", "MIDI data byte has no running status");
                if (!channelEvent(event, running, status, true, cursor, error)) {
                    return reject(error == "truncated" ? "truncated" : "bad-event", "invalid MIDI channel event");
                }
                file.events.push_back(std::move(event));
            } else if (status >= 0xf0) {
                return reject("bad-event", "unsupported MIDI system message");
            } else {
                running = status;
                if (!channelEvent(event, status, 0, false, cursor, error)) {
                    return reject(error == "truncated" ? "truncated" : "bad-event", "invalid MIDI channel event");
                }
                file.events.push_back(std::move(event));
            }
        }
        if (!ended) return reject("missing-end-of-track", "MIDI track is missing End of Track");
        if (!cursor.seek(chunkEnd, error)) return reject("truncated", "truncated MIDI track");
    }
    if (!cursor.exhausted()) return reject("trailing-data", "unexpected bytes after the last MIDI track");
    std::stable_sort(file.events.begin(), file.events.end(), [](const Event& left, const Event& right) {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.track != right.track) return left.track < right.track;
        return left.sequence < right.sequence;
    });
    resolveTempo(file);
    resolveTimeSignatures(file);
    ParseResult result;
    result.status = ParseStatus::ok;
    result.file = std::move(file);
    return result;
}

ParseResult parseFile(const std::filesystem::path& path, Limits limits) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return reject("truncated", "failed to open MIDI file");
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) return reject("truncated", "failed to read MIDI file");
    if (static_cast<std::uint64_t>(length) > limits.maxFileBytes) return reject("file-too-large", "MIDI file exceeds the size limit");
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) return reject("truncated", "failed to read MIDI file");
    return parse(bytes.data(), bytes.size(), limits);
}

std::vector<Event> select(const File& file, const Selection& selection) {
    std::vector<Event> chosen;
    for (const auto& event : file.events) {
        if (selection.track && event.track != *selection.track) continue;
        if (selection.channel && (!event.hasChannel || event.channel != *selection.channel)) continue;
        chosen.push_back(event);
    }
    return chosen;
}

std::vector<NoteStream> noteStreams(const File& file) {
    std::vector<NoteStream> streams;
    for (const auto& event : file.events) {
        if (event.kind != EventKind::noteOn || !event.hasChannel) continue;
        auto found = std::find_if(streams.begin(), streams.end(), [&](const NoteStream& stream) {
            return stream.track == event.track && stream.channel == event.channel;
        });
        if (found == streams.end()) streams.push_back({event.track, event.channel, 1});
        else ++found->noteOns;
    }
    return streams;
}

bool requiresExplicitStream(const File& file) { return noteStreams(file).size() > 1; }

CapabilityReport assess(const File& file, RenderMode mode, SynthCapabilities capabilities) {
    CapabilityReport report;
    for (const auto& event : file.events) {
        if (event.kind == EventKind::meta) {
            report.preserved.push_back({"meta", describe(event), event.tick, event.track});
            continue;
        }
        if (!affectsSound(event) || isSupported(event, capabilities)) continue;
        report.unsupported.push_back({"unsupported-event", describe(event), event.tick, event.track});
    }
    report.rejected = mode == RenderMode::strict && !report.unsupported.empty();
    return report;
}

TempoMap::TempoMap(const File& file) : ppq_(file.ppq == 0 ? 480 : file.ppq), points_(file.tempo) {
    if (points_.empty() || points_.front().tick != 0) points_.insert(points_.begin(), {0, kDefaultTempoUs});
    std::uint64_t numer = 0;
    segments_.push_back({points_.front().tick, points_.front().microsecondsPerQuarter, 0});
    for (std::size_t index = 1; index < points_.size(); ++index) {
        const auto& previous = points_[index - 1];
        const auto span = static_cast<std::uint64_t>(points_[index].tick - previous.tick);
        if (previous.microsecondsPerQuarter != 0 && span > std::numeric_limits<std::uint64_t>::max() / previous.microsecondsPerQuarter) {
            numer = std::numeric_limits<std::uint64_t>::max();
        } else {
            numer += span * previous.microsecondsPerQuarter;
        }
        segments_.push_back({points_[index].tick, points_[index].microsecondsPerQuarter, numer});
    }
}

std::optional<std::int64_t> TempoMap::sampleAtTick(std::uint32_t tick, std::uint32_t sampleRate) const {
    if (sampleRate == 0 || ppq_ == 0 || segments_.empty()) return std::nullopt;
    auto segment = segments_.begin();
    for (auto cursor = segments_.begin(); cursor != segments_.end(); ++cursor) {
        if (cursor->tick <= tick) segment = cursor;
        else break;
    }
    const auto span = static_cast<std::uint64_t>(tick - segment->tick);
    if (segment->usPerQuarter != 0 && span > std::numeric_limits<std::uint64_t>::max() / segment->usPerQuarter) return std::nullopt;
    const auto numer = segment->numer + span * segment->usPerQuarter;
    if (numer < segment->numer) return std::nullopt;
    const auto product = mul64(numer, sampleRate);
    const auto denom = static_cast<std::uint64_t>(ppq_) * 1000000ull;
    const auto sample = divRound(product, denom);
    if (!sample || *sample > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    return static_cast<std::int64_t>(*sample);
}

ScheduleResult schedule(const File& file, std::uint32_t sampleRate, std::uint32_t blockSize, const Selection& selection) {
    ScheduleResult result;
    if (sampleRate == 0 || blockSize == 0) {
        result.code = "invalid-schedule";
        result.message = "sample rate and block size must be positive";
        return result;
    }
    auto events = select(file, selection);
    std::stable_sort(events.begin(), events.end(), [](const Event& left, const Event& right) {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.track != right.track) return left.track < right.track;
        return left.sequence < right.sequence;
    });
    normalizeRetriggers(events);
    const TempoMap map(file);
    result.events.reserve(events.size());
    for (auto& event : events) {
        const auto sample = map.sampleAtTick(event.tick, sampleRate);
        if (!sample) {
            result.code = "sample-overflow";
            result.message = "event time does not fit in a sample index";
            result.events.clear();
            return result;
        }
        PlaybackEvent playback;
        playback.absoluteSample = *sample;
        playback.blockIndex = static_cast<std::uint64_t>(*sample) / blockSize;
        playback.sampleOffset = static_cast<std::uint32_t>(static_cast<std::uint64_t>(*sample) % blockSize);
        playback.source = std::move(event);
        result.events.push_back(std::move(playback));
    }
    result.status = ParseStatus::ok;
    return result;
}
} // namespace nodsynth::midi
