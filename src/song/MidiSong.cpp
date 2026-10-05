#include <nodsynth/song/SongDocument.h>

#include <algorithm>
#include <fstream>
#include <limits>

namespace nodsynth::song {
namespace {
struct OpenNote {
    std::uint8_t pitch{0};
    std::uint8_t velocity{0};
    std::uint32_t tick{0};
};

void write16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void write32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void writeVarlen(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::uint8_t bytes[4]{};
    int count = 0;
    bytes[count++] = static_cast<std::uint8_t>(value & 0x7f);
    value >>= 7;
    while (value != 0 && count < 4) {
        bytes[count++] = static_cast<std::uint8_t>((value & 0x7f) | 0x80);
        value >>= 7;
    }
    while (count > 0) out.push_back(bytes[--count]);
}

bool supportedPerformance(const midi::Event& event) {
    if (event.kind == midi::EventKind::pitchBend) return true;
    if (event.kind == midi::EventKind::controlChange) {
        return event.data1 == 64 || event.data1 == 120 || event.data1 == 123;
    }
    return false;
}

bool affectsSound(const midi::Event& event) {
    return event.kind != midi::EventKind::tempo && event.kind != midi::EventKind::timeSignature &&
           event.kind != midi::EventKind::meta && event.kind != midi::EventKind::noteOn &&
           event.kind != midi::EventKind::noteOff;
}

std::string nextId(int& counter, const char* prefix) { return std::string(prefix) + "-" + std::to_string(++counter); }

struct OutEvent {
    std::uint32_t tick{0};
    int order{0};
    std::vector<std::uint8_t> bytes;
};

void appendTrack(std::vector<std::uint8_t>& out, std::vector<OutEvent> events) {
    std::stable_sort(events.begin(), events.end(), [](const OutEvent& left, const OutEvent& right) {
        if (left.tick != right.tick) return left.tick < right.tick;
        return left.order < right.order;
    });
    std::vector<std::uint8_t> body;
    std::uint32_t previous = 0;
    for (const auto& event : events) {
        writeVarlen(body, event.tick - previous);
        previous = event.tick;
        body.insert(body.end(), event.bytes.begin(), event.bytes.end());
    }
    writeVarlen(body, 0);
    body.insert(body.end(), {0xff, 0x2f, 0x00});
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    write32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

int denominatorExponent(std::uint16_t denominator) {
    if (denominator == 0 || (denominator & (denominator - 1)) != 0) return -1;
    int exponent = 0;
    while (denominator > 1) {
        denominator = static_cast<std::uint16_t>(denominator >> 1);
        ++exponent;
    }
    return exponent;
}
} // namespace

ImportResult importMidi(const midi::File& file, const ImportOptions& options) {
    ImportResult result;
    if (file.ppq == 0) {
        result.code = "bad-midi";
        result.message = "MIDI file has no PPQ";
        return result;
    }
    SongDocument song;
    song.ppq = file.ppq;
    song.tempo.clear();
    for (const auto& point : file.tempo) song.tempo.push_back({point.tick, point.microsecondsPerQuarter});
    if (song.tempo.empty()) song.tempo.push_back({});
    song.timeSignatures.clear();
    for (const auto& point : file.timeSignatures) {
        song.timeSignatures.push_back({point.tick, point.numerator, point.denominator});
    }
    if (song.timeSignatures.empty()) song.timeSignatures.push_back({});
    for (const auto& diagnostic : file.diagnostics) {
        song.diagnostics.push_back({diagnostic.code, diagnostic.message, diagnostic.tick, diagnostic.track});
    }

    struct Stream {
        std::uint16_t track{0};
        std::uint8_t channel{0};
        std::vector<Note> notes;
        std::vector<PerformanceEvent> performance;
    };
    std::vector<Stream> streams;
    auto streamFor = [&](std::uint16_t track, std::uint8_t channel) -> Stream* {
        for (auto& stream : streams) {
            if (stream.track == track && stream.channel == channel) return &stream;
        }
        return nullptr;
    };
    std::vector<midi::Event> events = file.events;
    std::stable_sort(events.begin(), events.end(), [](const midi::Event& left, const midi::Event& right) {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.track != right.track) return left.track < right.track;
        return left.sequence < right.sequence;
    });

    for (const auto& event : events) {
        if (event.kind != midi::EventKind::noteOn || !event.hasChannel) continue;
        if (streamFor(event.track, event.channel) != nullptr) continue;
        streams.push_back({event.track, event.channel, {}, {}});
    }
    std::stable_sort(streams.begin(), streams.end(), [](const Stream& left, const Stream& right) {
        if (left.track != right.track) return left.track < right.track;
        return left.channel < right.channel;
    });
    std::vector<std::string> trackNames(file.trackCount);
    for (const auto& event : events) {
        if (event.kind != midi::EventKind::meta || event.metaType != 0x03 || event.track >= trackNames.size()) continue;
        if (trackNames[event.track].empty()) trackNames[event.track] = std::string(event.payload.begin(), event.payload.end());
    }
    std::vector<std::vector<OpenNote>> open(streams.size());
    int noteCounter = 0;
    int performanceCounter = 0;
    for (std::size_t index = 0; index < events.size();) {
        const auto tick = events[index].tick;
        const auto track = events[index].track;
        std::size_t end = index + 1;
        while (end < events.size() && events[end].tick == tick && events[end].track == track) ++end;
        struct Item {
            const midi::Event* event;
            bool matchedOff{false};
        };
        std::vector<Item> group;
        for (std::size_t cursor = index; cursor < end; ++cursor) group.push_back({&events[cursor], false});
        for (auto& item : group) {
            if (item.event->kind != midi::EventKind::noteOff || !item.event->hasChannel) continue;
            auto* stream = streamFor(item.event->track, item.event->channel);
            if (stream == nullptr) continue;
            const auto streamIndex = static_cast<std::size_t>(stream - streams.data());
            auto& held = open[streamIndex];
            auto found = std::find_if(held.begin(), held.end(), [&](const OpenNote& note) { return note.pitch == item.event->data1; });
            if (found == held.end()) continue;
            Note note;
            note.id = "note-" + std::to_string(++noteCounter);
            note.tick = found->tick;
            note.duration = tick - found->tick;
            note.pitch = found->pitch;
            note.velocity = found->velocity;
            note.channel = item.event->channel;
            stream->notes.push_back(note);
            held.erase(found);
            item.matchedOff = true;
        }
        for (const auto& item : group) {
            const auto& event = *item.event;
            if (!event.hasChannel) continue;
            if (event.kind == midi::EventKind::noteOn) {
                auto* stream = streamFor(event.track, event.channel);
                if (stream == nullptr) {
                    streams.push_back({event.track, event.channel, {}, {}});
                    open.emplace_back();
                    stream = &streams.back();
                }
                const auto streamIndex = static_cast<std::size_t>(stream - streams.data());
                auto& held = open[streamIndex];
                auto existing = std::find_if(held.begin(), held.end(), [&](const OpenNote& note) { return note.pitch == event.data1; });
                if (existing != held.end()) {
                    Note note;
                    note.id = "note-" + std::to_string(++noteCounter);
                    note.tick = existing->tick;
                    note.duration = tick - existing->tick;
                    note.pitch = existing->pitch;
                    note.velocity = existing->velocity;
                    note.channel = event.channel;
                    stream->notes.push_back(note);
                    held.erase(existing);
                }
                held.push_back({event.data1, event.data2, tick});
            } else if (supportedPerformance(event)) {
                auto* stream = streamFor(event.track, event.channel);
                if (stream == nullptr) continue;
                PerformanceEvent performance;
                performance.id = "perf-" + std::to_string(++performanceCounter);
                performance.tick = tick;
                performance.kind = event.kind;
                performance.channel = event.channel;
                performance.data1 = event.data1;
                performance.data2 = event.data2;
                stream->performance.push_back(std::move(performance));
            } else if (affectsSound(event)) {
                PreservedEvent preserved;
                preserved.tick = event.tick;
                preserved.sourceTrack = event.track;
                preserved.sequence = event.sequence;
                preserved.kind = event.kind;
                preserved.hasChannel = event.hasChannel;
                preserved.channel = event.channel;
                preserved.data1 = event.data1;
                preserved.data2 = event.data2;
                preserved.metaType = event.metaType;
                preserved.affectsSound = true;
                preserved.payload = event.payload;
                song.preserved.push_back(std::move(preserved));
            }
        }
        for (auto& item : group) {
            if (item.event->kind != midi::EventKind::noteOff || !item.event->hasChannel || item.matchedOff) continue;
            auto* stream = streamFor(item.event->track, item.event->channel);
            if (stream == nullptr) continue;
            const auto streamIndex = static_cast<std::size_t>(stream - streams.data());
            auto& held = open[streamIndex];
            auto found = std::find_if(held.begin(), held.end(), [&](const OpenNote& note) { return note.pitch == item.event->data1; });
            if (found == held.end()) continue;
            Note note;
            note.id = "note-" + std::to_string(++noteCounter);
            note.tick = found->tick;
            note.duration = tick - found->tick;
            note.pitch = found->pitch;
            note.velocity = found->velocity;
            note.channel = item.event->channel;
            stream->notes.push_back(note);
            held.erase(found);
        }
        index = end;
    }
    for (std::size_t streamIndex = 0; streamIndex < streams.size(); ++streamIndex) {
        for (const auto& held : open[streamIndex]) {
            Note note;
            note.id = "note-" + std::to_string(++noteCounter);
            note.tick = held.tick;
            note.duration = file.endTick >= held.tick ? file.endTick - held.tick : 0;
            note.pitch = held.pitch;
            note.velocity = held.velocity;
            note.channel = streams[streamIndex].channel;
            streams[streamIndex].notes.push_back(note);
        }
    }

    int trackCounter = 0;
    int clipCounter = 0;
    int instrumentCounter = 0;
    int resourceCounter = 0;
    for (const auto& stream : streams) {
        Track track;
        track.id = nextId(trackCounter, "track");
        if (stream.track < trackNames.size() && !trackNames[stream.track].empty()) track.name = trackNames[stream.track];
        else track.name = "Track " + std::to_string(stream.track) + " ch " + std::to_string(stream.channel);
        track.order = static_cast<int>(song.tracks.size());
        track.sourceTrack = stream.track;
        track.sourceChannel = stream.channel;
        Clip clip;
        clip.id = nextId(clipCounter, "clip");
        clip.notes = stream.notes;
        for (const auto& note : clip.notes) clip.length = std::max(clip.length, note.tick + note.duration);
        clip.length = std::max(clip.length, file.endTick);
        track.clips.push_back(std::move(clip));
        track.performance = stream.performance;
        const auto map = std::find_if(options.maps.begin(), options.maps.end(), [&](const StreamMap& candidate) {
            return candidate.sourceTrack == stream.track && candidate.sourceChannel == stream.channel;
        });
        if (map != options.maps.end()) {
            if (!map->name.empty()) track.name = map->name;
            if (!map->patchPath.empty()) {
                Resource resource;
                resource.id = nextId(resourceCounter, "res");
                resource.path = map->patchPath;
                resource.hash = hashFile(map->patchPath);
                resource.kind = "patch";
                Instrument instrument;
                instrument.id = nextId(instrumentCounter, "inst");
                instrument.kind = InstrumentKind::nodsynth;
                instrument.resourceId = resource.id;
                instrument.name = track.name;
                track.instrumentId = instrument.id;
                song.resources.push_back(std::move(resource));
                song.instruments.push_back(std::move(instrument));
            }
        }
        song.tracks.push_back(std::move(track));
    }
    for (const auto& map : options.maps) {
        const auto found = std::find_if(song.tracks.begin(), song.tracks.end(), [&](const Track& track) {
            return track.sourceTrack == map.sourceTrack && track.sourceChannel == map.sourceChannel;
        });
        if (found == song.tracks.end()) {
            song.diagnostics.push_back(
                {"unknown-stream",
                 "mapping does not match a note stream",
                 0,
                 map.sourceTrack,
                 std::to_string(map.sourceTrack) + ":" + std::to_string(map.sourceChannel)});
        }
    }
    result.ok = true;
    result.song = std::move(song);
    result.message = "imported";
    return result;
}

std::vector<std::uint8_t> exportMidiBytes(const SongDocument& song, std::string& error) {
    if (song.ppq == 0) {
        error = "song ppq must be positive";
        return {};
    }
    std::vector<OutEvent> conductor;
        for (const auto& point : song.tempo) {
        if (point.microsecondsPerQuarter > 0xffffffu) {
            error = "tempo does not fit in a MIDI meta event";
            return {};
        }
        OutEvent event;
        event.tick = point.tick;
        event.bytes = {
            0xff, 0x51, 0x03, static_cast<std::uint8_t>(point.microsecondsPerQuarter >> 16),
            static_cast<std::uint8_t>(point.microsecondsPerQuarter >> 8), static_cast<std::uint8_t>(point.microsecondsPerQuarter)};
        conductor.push_back(std::move(event));
    }
    for (const auto& point : song.timeSignatures) {
        const auto exponent = denominatorExponent(point.denominator);
        if (exponent < 0) {
            error = "time signature denominator must be a power of two";
            return {};
        }
        OutEvent event;
        event.tick = point.tick;
        event.order = 1;
        event.bytes = {0xff, 0x58, 0x04, point.numerator, static_cast<std::uint8_t>(exponent), 24, 8};
        conductor.push_back(std::move(event));
    }
    std::vector<std::uint8_t> out{'M', 'T', 'h', 'd'};
    write32(out, 6);
    write16(out, 1);
    write16(out, static_cast<std::uint16_t>(song.tracks.size() + 1));
    write16(out, song.ppq);
    appendTrack(out, std::move(conductor));
    for (const auto& track : song.tracks) {
        std::vector<OutEvent> events;
        for (const auto& clip : track.clips) {
            for (const auto& note : clip.notes) {
                const auto start = static_cast<std::uint64_t>(clip.startTick) + note.tick;
                const auto stop = start + note.duration;
                if (start > 0xffffffffu || stop > 0xffffffffu) {
                    error = "a note does not fit in a MIDI tick";
                    return {};
                }
                OutEvent on;
                on.tick = static_cast<std::uint32_t>(start);
                on.order = 2;
                on.bytes = {
                    static_cast<std::uint8_t>(0x90 | (note.channel & 0x0f)), note.pitch,
                    static_cast<std::uint8_t>(note.velocity == 0 ? 1 : note.velocity)};
                OutEvent off;
                off.tick = static_cast<std::uint32_t>(stop);
                off.order = note.duration == 0 ? 3 : 0;
                off.bytes = {static_cast<std::uint8_t>(0x80 | (note.channel & 0x0f)), note.pitch, 0};
                events.push_back(std::move(on));
                events.push_back(std::move(off));
            }
        }
        for (const auto& performance : track.performance) {
            OutEvent event;
            event.tick = performance.tick;
            event.order = 1;
            if (performance.kind == midi::EventKind::pitchBend) {
                event.bytes = {static_cast<std::uint8_t>(0xe0 | performance.channel), performance.data1, performance.data2};
            } else if (performance.kind == midi::EventKind::controlChange) {
                event.bytes = {static_cast<std::uint8_t>(0xb0 | performance.channel), performance.data1, performance.data2};
            } else {
                continue;
            }
            events.push_back(std::move(event));
        }
        appendTrack(out, std::move(events));
    }
    return out;
}

std::vector<std::uint8_t> exportTrackMidi(const SongDocument& song, const std::string& trackId, std::string& error) {
    auto found = std::find_if(song.tracks.begin(), song.tracks.end(), [&](const Track& track) { return track.id == trackId; });
    if (found == song.tracks.end()) {
        error = "track was not found";
        return {};
    }
    SongDocument one = song;
    Track track = *found;
    one.tracks.clear();
    one.tracks.push_back(std::move(track));
    one.preserved.clear();
    return exportMidiBytes(one, error);
}

bool exportMidi(const SongDocument& song, const std::filesystem::path& path, std::string& error) {
    const auto bytes = exportMidiBytes(song, error);
    if (bytes.empty()) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "failed to write MIDI file";
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        error = "failed to write MIDI file";
        return false;
    }
    return true;
}

std::optional<std::int64_t> sampleAtTick(const SongDocument& song, std::uint32_t tick, std::uint32_t sampleRate) {
    midi::File file;
    file.ppq = song.ppq == 0 ? 480 : song.ppq;
    for (const auto& point : song.tempo) file.tempo.push_back({point.tick, point.microsecondsPerQuarter});
    return midi::TempoMap{file}.sampleAtTick(tick, sampleRate);
}

std::uint32_t endTick(const SongDocument& song) {
    std::uint64_t end = 0;
    if (song.songRangeEndTick) end = *song.songRangeEndTick;
    for (const auto& point : song.tempo) end = std::max<std::uint64_t>(end, point.tick);
    for (const auto& point : song.timeSignatures) end = std::max<std::uint64_t>(end, point.tick);
    for (const auto& event : song.preserved) end = std::max<std::uint64_t>(end, event.tick);
    for (const auto& track : song.tracks) {
        for (const auto& event : track.performance) end = std::max<std::uint64_t>(end, event.tick);
        for (const auto& clip : track.clips) {
            end = std::max(end, static_cast<std::uint64_t>(clip.startTick) + clip.length);
            for (const auto& note : clip.notes) {
                end = std::max(end, static_cast<std::uint64_t>(clip.startTick) + note.tick + note.duration);
            }
        }
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(end, 0xffffffffu));
}
} // namespace nodsynth::song
