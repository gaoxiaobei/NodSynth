#include <nodsynth/song/SongDocument.h>
#include <nodsynth/song/Presets.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nodsynth::song {
namespace {
std::string hex64(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int index = 15; index >= 0; --index) {
        text[static_cast<std::size_t>(index)] = digits[value & 0xf];
        value >>= 4;
    }
    return text;
}

bool readText(const std::filesystem::path& path, std::string& text, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "failed to open the song file";
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

bool replaceFile(const std::filesystem::path& temporary, const std::filesystem::path& target, std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "failed to replace the song file";
        return false;
    }
    return true;
#else
    std::error_code failure;
    std::filesystem::rename(temporary, target, failure);
    if (failure) {
        error = failure.message();
        return false;
    }
    return true;
#endif
}

bool finiteNumber(const persist::Json* value, double& number) {
    if (value == nullptr || value->kind() != persist::Json::Kind::number) return false;
    number = value->asNumber();
    return std::isfinite(number);
}

bool readU32(const persist::Json* value, std::uint32_t& out) {
    double number = 0;
    if (!finiteNumber(value, number) || number < 0.0 || number > 4294967295.0) return false;
    out = static_cast<std::uint32_t>(std::llround(number));
    return std::fabs(number - static_cast<double>(out)) <= 1e-6;
}

bool readU16(const persist::Json* value, std::uint16_t& out) {
    std::uint32_t wide = 0;
    if (!readU32(value, wide) || wide > 65535) return false;
    out = static_cast<std::uint16_t>(wide);
    return true;
}

bool readU8(const persist::Json* value, std::uint8_t& out) {
    std::uint32_t wide = 0;
    if (!readU32(value, wide) || wide > 255) return false;
    out = static_cast<std::uint8_t>(wide);
    return true;
}

const char* kindName(InstrumentKind kind) {
    switch (kind) {
    case InstrumentKind::nodsynth: return "nodsynth";
    case InstrumentKind::externalCli: return "external-cli";
    case InstrumentKind::vst3: return "vst3";
    }
    return "nodsynth";
}

std::optional<InstrumentKind> kindFromName(const std::string& name) {
    if (name == "nodsynth") return InstrumentKind::nodsynth;
    if (name == "external-cli") return InstrumentKind::externalCli;
    if (name == "vst3") return InstrumentKind::vst3;
    return std::nullopt;
}

std::optional<midi::EventKind> eventKindFromName(const std::string& name) {
    if (name == "note-on") return midi::EventKind::noteOn;
    if (name == "note-off") return midi::EventKind::noteOff;
    if (name == "pitch-bend") return midi::EventKind::pitchBend;
    if (name == "control-change") return midi::EventKind::controlChange;
    if (name == "program-change") return midi::EventKind::programChange;
    if (name == "channel-pressure") return midi::EventKind::channelPressure;
    if (name == "poly-pressure") return midi::EventKind::polyPressure;
    if (name == "tempo") return midi::EventKind::tempo;
    if (name == "time-signature") return midi::EventKind::timeSignature;
    if (name == "sysex") return midi::EventKind::sysex;
    if (name == "meta") return midi::EventKind::meta;
    return std::nullopt;
}

persist::Json diagnosticJson(const Diagnostic& diagnostic) {
    persist::Json item = persist::Json::object();
    item.set("code", persist::Json::string(diagnostic.code));
    item.set("message", persist::Json::string(diagnostic.message));
    item.set("tick", persist::Json::number(diagnostic.tick));
    item.set("track", persist::Json::number(diagnostic.track));
    if (!diagnostic.objectId.empty()) item.set("object", persist::Json::string(diagnostic.objectId));
    return item;
}

void addUnique(std::vector<std::string>& ids, const std::string& id, const char* what, Validation& result) {
    if (id.empty()) {
        result.diagnostics.push_back({"empty-id", std::string(what) + " is missing an id"});
        return;
    }
    if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
        result.diagnostics.push_back({"duplicate-id", std::string(what) + " id is duplicated", 0, -1, id});
        return;
    }
    ids.push_back(id);
}

Track* findTrack(SongDocument& song, const std::string& id) {
    for (auto& track : song.tracks) {
        if (track.id == id) return &track;
    }
    return nullptr;
}

Clip* findClip(Track& track, const std::string& id) {
    for (auto& clip : track.clips) {
        if (clip.id == id) return &clip;
    }
    return nullptr;
}

std::string freshId(const SongDocument& song, const char* prefix) {
    std::uint32_t highest = 0;
    const auto consider = [&](const std::string& id) {
        const std::string stem = std::string(prefix) + "-";
        if (id.rfind(stem, 0) != 0) return;
        try {
            const auto value = static_cast<std::uint32_t>(std::stoul(id.substr(stem.size())));
            highest = std::max(highest, value);
        } catch (...) {}
    };
    for (const auto& resource : song.resources) consider(resource.id);
    for (const auto& instrument : song.instruments) consider(instrument.id);
    for (const auto& track : song.tracks) {
        consider(track.id);
        for (const auto& clip : track.clips) {
            consider(clip.id);
            for (const auto& note : clip.notes) consider(note.id);
        }
        for (const auto& event : track.performance) consider(event.id);
    }
    return std::string(prefix) + "-" + std::to_string(highest + 1);
}

bool applyOne(SongDocument& song, const persist::Json& command, std::string& error, persist::Json& idMap) {
    (void)idMap;
    const auto* op = command.find("op");
    if (op == nullptr || op->kind() != persist::Json::Kind::string) {
        error = "a command is missing op";
        return false;
    }
    const auto name = op->asString();
    if (name == "set-instrument") {
        const auto* trackId = command.find("track");
        const auto* patch = command.find("patch");
        if (trackId == nullptr || patch == nullptr) {
            error = "set-instrument requires track and patch";
            return false;
        }
        std::filesystem::path file = patch->asString();
        return bindPatch(song, trackId->asString(), patch->asString(), file, error);
    }
    if (name == "set-gain" || name == "set-pan") {
        const auto* trackId = command.find("track");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr) {
            error = "track was not found";
            return false;
        }
        double value = 0;
        if (!finiteNumber(command.find(name == "set-gain" ? "gain" : "pan"), value)) {
            error = "gain or pan is not a finite number";
            return false;
        }
        if (name == "set-gain") {
            if (value < 0.0) {
                error = "gain must be non-negative";
                return false;
            }
            track->gain = value;
        } else {
            if (value < -1.0 || value > 1.0) {
                error = "pan must be from -1 to 1";
                return false;
            }
            track->pan = value;
        }
        return true;
    }
    if (name == "set-gain-automation") {
        const auto* trackId = command.find("track");
        const auto* points = command.find("points");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr || points == nullptr || points->kind() != persist::Json::Kind::array) {
            error = "set-gain-automation requires a track and points";
            return false;
        }
        std::vector<GainPoint> automation;
        for (const auto& point : points->asArray()) {
            GainPoint gain;
            double value = 0;
            if (!readU32(point.find("tick"), gain.tick) || !finiteNumber(point.find("gain"), value) || value < 0.0) {
                error = "a gain point is invalid";
                return false;
            }
            gain.gain = value;
            automation.push_back(gain);
        }
        std::stable_sort(automation.begin(), automation.end(), [](const GainPoint& left, const GainPoint& right) {
            return left.tick < right.tick;
        });
        track->gainAutomation = std::move(automation);
        return true;
    }
    if (name == "set-parameter-automation") {
        const auto* trackId = command.find("track");
        const auto* parameter = command.find("parameter");
        const auto* points = command.find("points");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr || parameter == nullptr || parameter->kind() != persist::Json::Kind::string || parameter->asString().empty() ||
            points == nullptr || points->kind() != persist::Json::Kind::array) {
            error = "set-parameter-automation requires a track, parameter, and points";
            return false;
        }
        ParameterLane lane;
        lane.id = parameter->asString();
        for (const auto& point : points->asArray()) {
            ParameterPoint value;
            double normalized = 0;
            if (!readU32(point.find("tick"), value.tick) || !finiteNumber(point.find("value"), normalized) || normalized < 0.0 || normalized > 1.0) {
                error = "a parameter point is invalid";
                return false;
            }
            value.value = normalized;
            lane.points.push_back(value);
        }
        std::stable_sort(lane.points.begin(), lane.points.end(), [](const ParameterPoint& left, const ParameterPoint& right) {
            return left.tick < right.tick;
        });
        const auto existing = std::find_if(track->parameterAutomation.begin(), track->parameterAutomation.end(), [&](const ParameterLane& candidate) {
            return candidate.id == lane.id;
        });
        if (existing == track->parameterAutomation.end()) track->parameterAutomation.push_back(std::move(lane));
        else *existing = std::move(lane);
        return true;
    }
    if (name == "move-clip") {
        const auto* trackId = command.find("track");
        const auto* clipId = command.find("clip");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        auto* clip = track == nullptr || clipId == nullptr ? nullptr : findClip(*track, clipId->asString());
        if (clip == nullptr || !readU32(command.find("startTick"), clip->startTick)) {
            error = "move-clip requires a track, clip, and startTick";
            return false;
        }
        return true;
    }
    if (name == "move-note") {
        const auto* trackId = command.find("track");
        const auto* noteId = command.find("note");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr || noteId == nullptr) {
            error = "move-note requires a track and note";
            return false;
        }
        Note* note = nullptr;
        for (auto& clip : track->clips) {
            for (auto& candidate : clip.notes) {
                if (candidate.id == noteId->asString()) note = &candidate;
            }
        }
        if (note == nullptr) {
            error = "note was not found";
            return false;
        }
        if (const auto* tick = command.find("tick")) {
            if (!readU32(tick, note->tick)) {
                error = "note tick is invalid";
                return false;
            }
        }
        if (const auto* pitch = command.find("pitch")) {
            if (!readU8(pitch, note->pitch)) {
                error = "note pitch is invalid";
                return false;
            }
        }
        return true;
    }
    if (name == "add-note") {
        const auto* trackId = command.find("track");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr) {
            error = "track was not found";
            return false;
        }
        Clip* clip = nullptr;
        if (const auto* clipId = command.find("clip"); clipId != nullptr && clipId->kind() == persist::Json::Kind::string) {
            clip = findClip(*track, clipId->asString());
        } else if (!track->clips.empty()) {
            clip = &track->clips.front();
        }
        if (clip == nullptr) {
            error = "clip was not found";
            return false;
        }
        Note note;
        const auto* id = command.find("id");
        note.id = id != nullptr && id->kind() == persist::Json::Kind::string && !id->asString().empty() ? id->asString()
                                                                                                         : freshId(song, "note");
        if (!readU32(command.find("tick"), note.tick) || !readU32(command.find("duration"), note.duration) ||
            !readU8(command.find("pitch"), note.pitch) || !readU8(command.find("velocity"), note.velocity)) {
            error = "add-note requires tick, duration, pitch, and velocity";
            return false;
        }
        if (clip->length != 0 && note.tick + note.duration > clip->length) {
            error = "note extends past the clip length";
            return false;
        }
        if (const auto* channel = command.find("channel")) {
            if (!readU8(channel, note.channel) || note.channel > 15) {
                error = "note channel is invalid";
                return false;
            }
        }
        for (const auto& existing : song.tracks) {
            for (const auto& existingClip : existing.clips) {
                for (const auto& existingNote : existingClip.notes) {
                    if (existingNote.id == note.id) {
                        error = "note id already exists";
                        return false;
                    }
                }
            }
        }
        clip->notes.push_back(std::move(note));
        return true;
    }
    if (name == "delete-note") {
        const auto* trackId = command.find("track");
        const auto* noteId = command.find("note");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr || noteId == nullptr) {
            error = "delete-note requires a track and note";
            return false;
        }
        for (auto& clip : track->clips) {
            const auto found = std::find_if(clip.notes.begin(), clip.notes.end(), [&](const Note& note) {
                return note.id == noteId->asString();
            });
            if (found != clip.notes.end()) {
                clip.notes.erase(found);
                return true;
            }
        }
        error = "note was not found";
        return false;
    }
    if (name == "create-track") {
        Track track;
        const auto* id = command.find("id");
        track.id = id != nullptr && id->kind() == persist::Json::Kind::string && !id->asString().empty() ? id->asString()
                                                                                                       : freshId(song, "track");
        if (findTrack(song, track.id) != nullptr) {
            error = "track id already exists";
            return false;
        }
        if (const auto* trackName = command.find("name"); trackName != nullptr) track.name = trackName->asString();
        else track.name = track.id;
        std::uint32_t order = static_cast<std::uint32_t>(song.tracks.size());
        if (const auto* value = command.find("order")) {
            if (!readU32(value, order)) {
                error = "track order is invalid";
                return false;
            }
        }
        track.order = static_cast<int>(order);
        song.tracks.push_back(std::move(track));
        idMap.set(song.tracks.back().id, persist::Json::string(song.tracks.back().id));
        if (id != nullptr && id->kind() == persist::Json::Kind::string) idMap.set(id->asString(), persist::Json::string(song.tracks.back().id));
        return true;
    }
    if (name == "delete-track") {
        const auto* trackId = command.find("track");
        if (trackId == nullptr) {
            error = "delete-track requires a track";
            return false;
        }
        const auto found = std::find_if(song.tracks.begin(), song.tracks.end(), [&](const Track& track) {
            return track.id == trackId->asString();
        });
        if (found == song.tracks.end()) {
            error = "track was not found";
            return false;
        }
        song.tracks.erase(found);
        return true;
    }
    if (name == "rename-track") {
        const auto* trackId = command.find("track");
        const auto* trackName = command.find("name");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr || trackName == nullptr) {
            error = "rename-track requires a track and name";
            return false;
        }
        track->name = trackName->asString();
        return true;
    }
    if (name == "reorder-track") {
        const auto* trackId = command.find("track");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        std::uint32_t order = 0;
        if (track == nullptr || !readU32(command.find("order"), order)) {
            error = "reorder-track requires a track and order";
            return false;
        }
        track->order = static_cast<int>(order);
        return true;
    }
    if (name == "create-clip") {
        const auto* trackId = command.find("track");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr) {
            error = "track was not found";
            return false;
        }
        Clip clip;
        const auto* id = command.find("id");
        clip.id = id != nullptr && id->kind() == persist::Json::Kind::string && !id->asString().empty() ? id->asString()
                                                                                                      : freshId(song, "clip");
        for (const auto& existing : song.tracks) {
            for (const auto& existingClip : existing.clips) {
                if (existingClip.id == clip.id) {
                    error = "clip id already exists";
                    return false;
                }
            }
        }
        if (!readU32(command.find("startTick"), clip.startTick)) clip.startTick = 0;
        if (!readU32(command.find("length"), clip.length) || clip.length == 0) clip.length = song.ppq * 4;
        track->clips.push_back(clip);
        idMap.set(clip.id, persist::Json::string(clip.id));
        return true;
    }
    if (name == "delete-clip") {
        const auto* trackId = command.find("track");
        const auto* clipId = command.find("clip");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr || clipId == nullptr) {
            error = "delete-clip requires a track and clip";
            return false;
        }
        const auto found = std::find_if(track->clips.begin(), track->clips.end(), [&](const Clip& clip) {
            return clip.id == clipId->asString();
        });
        if (found == track->clips.end()) {
            error = "clip was not found";
            return false;
        }
        track->clips.erase(found);
        return true;
    }
    if (name == "duplicate-clip") {
        const auto* trackId = command.find("track");
        const auto* clipId = command.find("clip");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        auto* clip = track == nullptr || clipId == nullptr ? nullptr : findClip(*track, clipId->asString());
        if (clip == nullptr) {
            error = "clip was not found";
            return false;
        }
        Clip copy = *clip;
        const auto* id = command.find("id");
        copy.id = id != nullptr && id->kind() == persist::Json::Kind::string && !id->asString().empty() ? id->asString()
                                                                                                      : freshId(song, "clip");
        if (const auto* start = command.find("startTick")) {
            if (!readU32(start, copy.startTick)) {
                error = "clip startTick is invalid";
                return false;
            }
        } else {
            copy.startTick = clip->startTick + clip->length;
        }
        for (auto& note : copy.notes) note.id = freshId(song, "note");
        track->clips.push_back(copy);
        idMap.set(copy.id, persist::Json::string(copy.id));
        return true;
    }
    if (name == "add-notes") {
        const auto* trackId = command.find("track");
        const auto* notes = command.find("notes");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr || notes == nullptr || notes->kind() != persist::Json::Kind::array) {
            error = "add-notes requires a track and notes";
            return false;
        }
        Clip* clip = nullptr;
        if (const auto* clipId = command.find("clip"); clipId != nullptr) clip = findClip(*track, clipId->asString());
        else if (!track->clips.empty()) clip = &track->clips.front();
        if (clip == nullptr) {
            error = "clip was not found";
            return false;
        }
        for (const auto& item : notes->asArray()) {
            persist::Json one = persist::Json::object();
            one.set("op", persist::Json::string("add-note"));
            one.set("track", persist::Json::string(track->id));
            one.set("clip", persist::Json::string(clip->id));
            if (const auto* id = item.find("id")) one.set("id", *id);
            if (const auto* tick = item.find("tick")) one.set("tick", *tick);
            if (const auto* duration = item.find("duration")) one.set("duration", *duration);
            if (const auto* pitch = item.find("pitch")) one.set("pitch", *pitch);
            if (const auto* velocity = item.find("velocity")) one.set("velocity", *velocity);
            if (const auto* channel = item.find("channel")) one.set("channel", *channel);
            if (!applyOne(song, one, error, idMap)) return false;
        }
        return true;
    }
    if (name == "update-note") {
        const auto* trackId = command.find("track");
        const auto* noteId = command.find("note");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr || noteId == nullptr) {
            error = "update-note requires a track and note";
            return false;
        }
        for (auto& clip : track->clips) {
            for (auto& note : clip.notes) {
                if (note.id != noteId->asString()) continue;
                if (const auto* tick = command.find("tick"); tick != nullptr && !readU32(tick, note.tick)) {
                    error = "note tick is invalid";
                    return false;
                }
                if (const auto* duration = command.find("duration"); duration != nullptr && !readU32(duration, note.duration)) {
                    error = "note duration is invalid";
                    return false;
                }
                if (const auto* pitch = command.find("pitch"); pitch != nullptr && !readU8(pitch, note.pitch)) {
                    error = "note pitch is invalid";
                    return false;
                }
                if (const auto* velocity = command.find("velocity"); velocity != nullptr && !readU8(velocity, note.velocity)) {
                    error = "note velocity is invalid";
                    return false;
                }
                if (clip.length != 0 && note.tick + note.duration > clip.length) {
                    error = "note extends past the clip length";
                    return false;
                }
                return true;
            }
        }
        error = "note was not found";
        return false;
    }
    if (name == "delete-notes") {
        const auto* trackId = command.find("track");
        const auto* notes = command.find("notes");
        auto* track = trackId == nullptr ? nullptr : findTrack(song, trackId->asString());
        if (track == nullptr || notes == nullptr || notes->kind() != persist::Json::Kind::array) {
            error = "delete-notes requires a track and notes";
            return false;
        }
        for (const auto& item : notes->asArray()) {
            persist::Json one = persist::Json::object();
            one.set("op", persist::Json::string("delete-note"));
            one.set("track", persist::Json::string(track->id));
            one.set("note", item.kind() == persist::Json::Kind::string ? item : persist::Json::string(item.find("id") ? item.find("id")->asString() : ""));
            if (!applyOne(song, one, error, idMap)) return false;
        }
        return true;
    }
    if (name == "set-tempo") {
        song.tempo.clear();
        if (const auto* points = command.find("points"); points != nullptr && points->kind() == persist::Json::Kind::array) {
            for (const auto& item : points->asArray()) {
                TempoPoint point;
                if (!readU32(item.find("tick"), point.tick)) {
                    error = "tempo tick is invalid";
                    return false;
                }
                if (const auto* bpm = item.find("bpm"); bpm != nullptr) {
                    double value = 0;
                    if (!finiteNumber(bpm, value) || value <= 0) {
                        error = "tempo bpm is invalid";
                        return false;
                    }
                    point.microsecondsPerQuarter = static_cast<std::uint32_t>(std::llround(60000000.0 / value));
                } else if (!readU32(item.find("microsecondsPerQuarter"), point.microsecondsPerQuarter) ||
                           point.microsecondsPerQuarter == 0) {
                    error = "tempo is invalid";
                    return false;
                }
                song.tempo.push_back(point);
            }
        } else {
            TempoPoint point;
            double bpm = 0;
            if (!finiteNumber(command.find("bpm"), bpm) || bpm <= 0) {
                error = "set-tempo requires bpm or points";
                return false;
            }
            point.microsecondsPerQuarter = static_cast<std::uint32_t>(std::llround(60000000.0 / bpm));
            song.tempo.push_back(point);
        }
        if (song.tempo.empty() || song.tempo.front().tick != 0) {
            error = "tempo map must start at tick 0";
            return false;
        }
        return true;
    }
    if (name == "set-time-signature") {
        TimeSignaturePoint point;
        std::uint32_t numerator = 4;
        if (const auto* tick = command.find("tick"); tick != nullptr && !readU32(tick, point.tick)) {
            error = "time signature tick is invalid";
            return false;
        }
        if (!readU32(command.find("numerator"), numerator) || numerator == 0 || numerator > 255 ||
            !readU16(command.find("denominator"), point.denominator) || point.denominator == 0) {
            error = "set-time-signature requires numerator and denominator";
            return false;
        }
        point.numerator = static_cast<std::uint8_t>(numerator);
        if (point.tick == 0) song.timeSignatures.clear();
        song.timeSignatures.push_back(point);
        return true;
    }
    if (name == "set-song-range") {
        std::uint32_t end = 0;
        if (const auto* bars = command.find("bars"); bars != nullptr) {
            std::uint32_t count = 0;
            if (!readU32(bars, count) || count == 0) {
                error = "song range bars are invalid";
                return false;
            }
            std::string barError;
            std::uint32_t startTick = 0;
            if (!barsToTicks(song, 1, count + 1, startTick, end, barError)) {
                error = barError;
                return false;
            }
        } else if (!readU32(command.find("endTick"), end)) {
            error = "set-song-range requires endTick or bars";
            return false;
        }
        song.songRangeEndTick = end;
        return true;
    }
    if (name == "bind-preset") {
        const auto* trackId = command.find("track");
        const auto* preset = command.find("preset");
        if (trackId == nullptr || preset == nullptr) {
            error = "bind-preset requires a track and preset";
            return false;
        }
        std::filesystem::path root = defaultPresetsRoot();
        if (const auto* presets = command.find("presetsRoot"); presets != nullptr) root = presets->asString();
        return bindPreset(song, trackId->asString(), preset->asString(), root, error);
    }
    error = "unknown command";
    return false;
}
} // namespace

std::string hashFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::uint64_t hash = 14695981039346656037ull;
    char buffer[4096];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const auto count = static_cast<std::size_t>(input.gcount());
        const auto* bytes = reinterpret_cast<const unsigned char*>(buffer);
        for (std::size_t index = 0; index < count; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    }
    return hex64(hash);
}

persist::Json toJson(const SongDocument& song) {
    persist::Json json = persist::Json::object();
    json.set("format", persist::Json::string("nodsynth.song"));
    json.set("formatVersion", persist::Json::number(song.version));
    json.set("revision", persist::Json::number(static_cast<double>(song.revision)));
    json.set("ppq", persist::Json::number(song.ppq));
    if (song.songRangeEndTick) json.set("songRangeEndTick", persist::Json::number(*song.songRangeEndTick));
    if (!song.sourceMidiHash.empty()) json.set("sourceMidiHash", persist::Json::string(song.sourceMidiHash));
    persist::Json tempo = persist::Json::array();
    for (const auto& point : song.tempo) {
        persist::Json item = persist::Json::object();
        item.set("tick", persist::Json::number(point.tick));
        item.set("microsecondsPerQuarter", persist::Json::number(point.microsecondsPerQuarter));
        tempo.push(std::move(item));
    }
    json.set("tempo", std::move(tempo));
    persist::Json signatures = persist::Json::array();
    for (const auto& point : song.timeSignatures) {
        persist::Json item = persist::Json::object();
        item.set("tick", persist::Json::number(point.tick));
        item.set("numerator", persist::Json::number(point.numerator));
        item.set("denominator", persist::Json::number(point.denominator));
        signatures.push(std::move(item));
    }
    json.set("timeSignatures", std::move(signatures));
    persist::Json resources = persist::Json::array();
    for (const auto& resource : song.resources) {
        persist::Json item = persist::Json::object();
        item.set("id", persist::Json::string(resource.id));
        item.set("path", persist::Json::string(resource.path));
        item.set("hash", persist::Json::string(resource.hash));
        item.set("kind", persist::Json::string(resource.kind));
        resources.push(std::move(item));
    }
    json.set("resources", std::move(resources));
    persist::Json instruments = persist::Json::array();
    for (const auto& instrument : song.instruments) {
        persist::Json item = persist::Json::object();
        item.set("id", persist::Json::string(instrument.id));
        item.set("kind", persist::Json::string(kindName(instrument.kind)));
        item.set("resource", persist::Json::string(instrument.resourceId));
        item.set("name", persist::Json::string(instrument.name));
        if (!instrument.adapter.empty()) item.set("adapter", persist::Json::string(instrument.adapter));
        instruments.push(std::move(item));
    }
    json.set("instruments", std::move(instruments));
    persist::Json tracks = persist::Json::array();
    for (const auto& track : song.tracks) {
        persist::Json item = persist::Json::object();
        item.set("id", persist::Json::string(track.id));
        item.set("name", persist::Json::string(track.name));
        item.set("instrument", persist::Json::string(track.instrumentId));
        item.set("gain", persist::Json::number(track.gain));
        item.set("pan", persist::Json::number(track.pan));
        item.set("order", persist::Json::number(track.order));
        if (track.sourceTrack) item.set("sourceTrack", persist::Json::number(*track.sourceTrack));
        if (track.sourceChannel) item.set("sourceChannel", persist::Json::number(*track.sourceChannel));
        persist::Json clips = persist::Json::array();
        for (const auto& clip : track.clips) {
            persist::Json clipJson = persist::Json::object();
            clipJson.set("id", persist::Json::string(clip.id));
            clipJson.set("startTick", persist::Json::number(clip.startTick));
            clipJson.set("length", persist::Json::number(clip.length));
            persist::Json notes = persist::Json::array();
            for (const auto& note : clip.notes) {
                persist::Json noteJson = persist::Json::object();
                noteJson.set("id", persist::Json::string(note.id));
                noteJson.set("tick", persist::Json::number(note.tick));
                noteJson.set("duration", persist::Json::number(note.duration));
                noteJson.set("pitch", persist::Json::number(note.pitch));
                noteJson.set("velocity", persist::Json::number(note.velocity));
                noteJson.set("channel", persist::Json::number(note.channel));
                notes.push(std::move(noteJson));
            }
            clipJson.set("notes", std::move(notes));
            clips.push(std::move(clipJson));
        }
        item.set("clips", std::move(clips));
        persist::Json automation = persist::Json::array();
        for (const auto& point : track.gainAutomation) {
            persist::Json pointJson = persist::Json::object();
            pointJson.set("tick", persist::Json::number(point.tick));
            pointJson.set("gain", persist::Json::number(point.gain));
            automation.push(std::move(pointJson));
        }
        item.set("gainAutomation", std::move(automation));
        persist::Json parameters = persist::Json::array();
        for (const auto& lane : track.parameterAutomation) {
            persist::Json laneJson = persist::Json::object();
            laneJson.set("id", persist::Json::string(lane.id));
            persist::Json points = persist::Json::array();
            for (const auto& point : lane.points) {
                persist::Json pointJson = persist::Json::object();
                pointJson.set("tick", persist::Json::number(point.tick));
                pointJson.set("value", persist::Json::number(point.value));
                points.push(std::move(pointJson));
            }
            laneJson.set("points", std::move(points));
            parameters.push(std::move(laneJson));
        }
        item.set("parameterAutomation", std::move(parameters));
        persist::Json performance = persist::Json::array();
        for (const auto& event : track.performance) {
            persist::Json eventJson = persist::Json::object();
            eventJson.set("id", persist::Json::string(event.id));
            eventJson.set("tick", persist::Json::number(event.tick));
            eventJson.set("kind", persist::Json::string(midi::eventKindName(event.kind)));
            eventJson.set("channel", persist::Json::number(event.channel));
            eventJson.set("data1", persist::Json::number(event.data1));
            eventJson.set("data2", persist::Json::number(event.data2));
            performance.push(std::move(eventJson));
        }
        item.set("performance", std::move(performance));
        tracks.push(std::move(item));
    }
    json.set("tracks", std::move(tracks));
    persist::Json preserved = persist::Json::array();
    for (const auto& event : song.preserved) {
        persist::Json item = persist::Json::object();
        item.set("tick", persist::Json::number(event.tick));
        item.set("sourceTrack", persist::Json::number(event.sourceTrack));
        item.set("sequence", persist::Json::number(event.sequence));
        item.set("kind", persist::Json::string(midi::eventKindName(event.kind)));
        item.set("hasChannel", persist::Json::boolean(event.hasChannel));
        item.set("channel", persist::Json::number(event.channel));
        item.set("data1", persist::Json::number(event.data1));
        item.set("data2", persist::Json::number(event.data2));
        item.set("metaType", persist::Json::number(event.metaType));
        item.set("affectsSound", persist::Json::boolean(event.affectsSound));
        persist::Json payload = persist::Json::array();
        for (const auto byte : event.payload) payload.push(persist::Json::number(byte));
        item.set("payload", std::move(payload));
        preserved.push(std::move(item));
    }
    json.set("preserved", std::move(preserved));
    persist::Json diagnostics = persist::Json::array();
    for (const auto& diagnostic : song.diagnostics) diagnostics.push(diagnosticJson(diagnostic));
    json.set("diagnostics", std::move(diagnostics));
    persist::Json requests = persist::Json::array();
    for (const auto& request : song.appliedRequests) requests.push(persist::Json::string(request));
    json.set("appliedRequests", std::move(requests));
    persist::Json undo = persist::Json::array();
    for (const auto& image : song.undoStack) undo.push(persist::Json::string(image));
    json.set("undo", std::move(undo));
    persist::Json redo = persist::Json::array();
    for (const auto& image : song.redoStack) redo.push(persist::Json::string(image));
    json.set("redo", std::move(redo));
    return json;
}

std::optional<SongDocument> songFromJson(const persist::Json& json, std::string& error) {
    const auto* format = json.find("format");
    const auto* version = json.find("formatVersion");
    if (format == nullptr || format->asString() != "nodsynth.song" || version == nullptr) {
        error = "the file is not a NodSynth song";
        return std::nullopt;
    }
    std::uint32_t versionNumber = 0;
    if (!readU32(version, versionNumber) || versionNumber < static_cast<std::uint32_t>(kMinSupportedSongFormatVersion) ||
        versionNumber > static_cast<std::uint32_t>(kSongFormatVersion)) {
        error = "the song version is not supported";
        return std::nullopt;
    }
    SongDocument song;
    song.version = kSongFormatVersion;
    std::uint32_t revision = 1;
    if (const auto* value = json.find("revision"); value != nullptr && !readU32(value, revision)) {
        error = "song revision is invalid";
        return std::nullopt;
    }
    song.revision = revision;
    if (const auto* value = json.find("ppq"); value != nullptr && !readU16(value, song.ppq)) {
        error = "song ppq is invalid";
        return std::nullopt;
    }
    if (const auto* value = json.find("sourceMidiHash"); value != nullptr) song.sourceMidiHash = value->asString();
    if (const auto* value = json.find("songRangeEndTick"); value != nullptr) {
        std::uint32_t end = 0;
        if (!readU32(value, end)) {
            error = "song range is invalid";
            return std::nullopt;
        }
        song.songRangeEndTick = end;
    }
    if (const auto* tempo = json.find("tempo"); tempo != nullptr) {
        if (tempo->kind() != persist::Json::Kind::array) {
            error = "tempo must be an array";
            return std::nullopt;
        }
        for (const auto& item : tempo->asArray()) {
            TempoPoint point;
            if (!readU32(item.find("tick"), point.tick) || !readU32(item.find("microsecondsPerQuarter"), point.microsecondsPerQuarter) ||
                point.microsecondsPerQuarter == 0) {
                error = "a tempo point is invalid";
                return std::nullopt;
            }
            song.tempo.push_back(point);
        }
    }
    if (song.tempo.empty()) song.tempo.push_back({});
    if (const auto* signatures = json.find("timeSignatures"); signatures != nullptr) {
        if (signatures->kind() != persist::Json::Kind::array) {
            error = "time signatures must be an array";
            return std::nullopt;
        }
        for (const auto& item : signatures->asArray()) {
            TimeSignaturePoint point;
            std::uint32_t numerator = 4;
            if (!readU32(item.find("tick"), point.tick) || !readU32(item.find("numerator"), numerator) || numerator == 0 ||
                numerator > 255 || !readU16(item.find("denominator"), point.denominator) || point.denominator == 0) {
                error = "a time signature is invalid";
                return std::nullopt;
            }
            point.numerator = static_cast<std::uint8_t>(numerator);
            song.timeSignatures.push_back(point);
        }
    }
    if (song.timeSignatures.empty()) song.timeSignatures.push_back({});
    const auto readObjects = [&](const char* key, auto&& reader) {
        const auto* items = json.find(key);
        if (items == nullptr) return true;
        if (items->kind() != persist::Json::Kind::array) {
            error = std::string(key) + " must be an array";
            return false;
        }
        for (const auto& item : items->asArray()) {
            if (!reader(item)) return false;
        }
        return true;
    };
    if (!readObjects("resources", [&](const persist::Json& item) {
            Resource resource;
            const auto* id = item.find("id");
            const auto* path = item.find("path");
            if (id == nullptr || path == nullptr) {
                error = "a resource is incomplete";
                return false;
            }
            resource.id = id->asString();
            resource.path = path->asString();
            if (const auto* hash = item.find("hash")) resource.hash = hash->asString();
            if (const auto* kind = item.find("kind"); kind != nullptr && !kind->asString().empty()) resource.kind = kind->asString();
            song.resources.push_back(std::move(resource));
            return true;
        })) {
        return std::nullopt;
    }
    if (!readObjects("instruments", [&](const persist::Json& item) {
            Instrument instrument;
            const auto* id = item.find("id");
            const auto* kind = item.find("kind");
            if (id == nullptr || kind == nullptr) {
                error = "an instrument is incomplete";
                return false;
            }
            const auto parsed = kindFromName(kind->asString());
            if (!parsed) {
                error = "an instrument kind is unknown";
                return false;
            }
            instrument.id = id->asString();
            instrument.kind = *parsed;
            if (const auto* resource = item.find("resource")) instrument.resourceId = resource->asString();
            if (const auto* name = item.find("name")) instrument.name = name->asString();
            if (const auto* adapter = item.find("adapter")) instrument.adapter = adapter->asString();
            song.instruments.push_back(std::move(instrument));
            return true;
        })) {
        return std::nullopt;
    }
    if (!readObjects("tracks", [&](const persist::Json& item) {
            Track track;
            const auto* id = item.find("id");
            if (id == nullptr) {
                error = "a track is missing its id";
                return false;
            }
            track.id = id->asString();
            if (const auto* name = item.find("name")) track.name = name->asString();
            if (const auto* instrument = item.find("instrument")) track.instrumentId = instrument->asString();
            double gain = 1;
            double pan = 0;
            if (const auto* value = item.find("gain"); value != nullptr && (!finiteNumber(value, gain) || gain < 0.0)) {
                error = "track gain is invalid";
                return false;
            }
            if (const auto* value = item.find("pan"); value != nullptr && (!finiteNumber(value, pan) || pan < -1.0 || pan > 1.0)) {
                error = "track pan is invalid";
                return false;
            }
            track.gain = gain;
            track.pan = pan;
            if (const auto* value = item.find("order"); value != nullptr) {
                std::uint32_t order = 0;
                if (!readU32(value, order) || order > 2147483647u) {
                    error = "track order is invalid";
                    return false;
                }
                track.order = static_cast<int>(order);
            }
            std::uint16_t sourceTrack = 0;
            std::uint8_t sourceChannel = 0;
            if (const auto* value = item.find("sourceTrack")) {
                if (!readU16(value, sourceTrack)) {
                    error = "source track is invalid";
                    return false;
                }
                track.sourceTrack = sourceTrack;
            }
            if (const auto* value = item.find("sourceChannel")) {
                if (!readU8(value, sourceChannel) || sourceChannel > 15) {
                    error = "source channel is invalid";
                    return false;
                }
                track.sourceChannel = sourceChannel;
            }
            if (const auto* clips = item.find("clips"); clips != nullptr) {
                if (clips->kind() != persist::Json::Kind::array) {
                    error = "clips must be an array";
                    return false;
                }
                for (const auto& clipJson : clips->asArray()) {
                    Clip clip;
                    const auto* clipId = clipJson.find("id");
                    if (clipId == nullptr || !readU32(clipJson.find("startTick"), clip.startTick)) {
                        error = "a clip is incomplete";
                        return false;
                    }
                    clip.id = clipId->asString();
                    if (const auto* length = clipJson.find("length"); length != nullptr && !readU32(length, clip.length)) {
                        error = "a clip length is invalid";
                        return false;
                    }
                    if (const auto* notes = clipJson.find("notes"); notes != nullptr) {
                        if (notes->kind() != persist::Json::Kind::array) {
                            error = "notes must be an array";
                            return false;
                        }
                        for (const auto& noteJson : notes->asArray()) {
                            Note note;
                            const auto* noteId = noteJson.find("id");
                            if (noteId == nullptr || !readU32(noteJson.find("tick"), note.tick) ||
                                !readU32(noteJson.find("duration"), note.duration) || !readU8(noteJson.find("pitch"), note.pitch) ||
                                !readU8(noteJson.find("velocity"), note.velocity) || !readU8(noteJson.find("channel"), note.channel) ||
                                note.channel > 15) {
                                error = "a note is invalid";
                                return false;
                            }
                            note.id = noteId->asString();
                            clip.notes.push_back(std::move(note));
                        }
                    }
                    if (clip.length == 0) {
                        for (const auto& note : clip.notes) {
                            clip.length = std::max(clip.length, note.tick + note.duration);
                        }
                        if (clip.length == 0) clip.length = song.ppq * 4;
                    }
                    track.clips.push_back(std::move(clip));
                }
            }
            if (const auto* automation = item.find("gainAutomation"); automation != nullptr) {
                if (automation->kind() != persist::Json::Kind::array) {
                    error = "gain automation must be an array";
                    return false;
                }
                for (const auto& pointJson : automation->asArray()) {
                    GainPoint point;
                    double value = 0;
                    if (!readU32(pointJson.find("tick"), point.tick) || !finiteNumber(pointJson.find("gain"), value) || value < 0.0) {
                        error = "a gain point is invalid";
                        return false;
                    }
                    point.gain = value;
                    track.gainAutomation.push_back(point);
                }
            }
            if (const auto* parameters = item.find("parameterAutomation"); parameters != nullptr) {
                if (parameters->kind() != persist::Json::Kind::array) {
                    error = "parameter automation must be an array";
                    return false;
                }
                for (const auto& laneJson : parameters->asArray()) {
                    const auto* laneId = laneJson.find("id");
                    const auto* points = laneJson.find("points");
                    if (laneId == nullptr || laneId->asString().empty() || points == nullptr || points->kind() != persist::Json::Kind::array) {
                        error = "a parameter lane is invalid";
                        return false;
                    }
                    ParameterLane lane;
                    lane.id = laneId->asString();
                    for (const auto& pointJson : points->asArray()) {
                        ParameterPoint point;
                        double value = 0;
                        if (!readU32(pointJson.find("tick"), point.tick) || !finiteNumber(pointJson.find("value"), value) || value < 0.0 ||
                            value > 1.0) {
                            error = "a parameter point is invalid";
                            return false;
                        }
                        point.value = value;
                        lane.points.push_back(point);
                    }
                    track.parameterAutomation.push_back(std::move(lane));
                }
            }
            if (const auto* performance = item.find("performance"); performance != nullptr) {
                if (performance->kind() != persist::Json::Kind::array) {
                    error = "performance events must be an array";
                    return false;
                }
                for (const auto& eventJson : performance->asArray()) {
                    PerformanceEvent event;
                    const auto* eventId = eventJson.find("id");
                    const auto* kind = eventJson.find("kind");
                    if (eventId == nullptr || kind == nullptr || !readU32(eventJson.find("tick"), event.tick) ||
                        !readU8(eventJson.find("channel"), event.channel) || !readU8(eventJson.find("data1"), event.data1) ||
                        !readU8(eventJson.find("data2"), event.data2)) {
                        error = "a performance event is invalid";
                        return false;
                    }
                    const auto parsed = eventKindFromName(kind->asString());
                    if (!parsed) {
                        error = "a performance event kind is unknown";
                        return false;
                    }
                    event.id = eventId->asString();
                    event.kind = *parsed;
                    track.performance.push_back(std::move(event));
                }
            }
            song.tracks.push_back(std::move(track));
            return true;
        })) {
        return std::nullopt;
    }
    if (!readObjects("preserved", [&](const persist::Json& item) {
            PreservedEvent event;
            const auto* kind = item.find("kind");
            std::uint32_t sequence = 0;
            if (kind == nullptr || !readU32(item.find("tick"), event.tick) || !readU32(item.find("sequence"), sequence)) {
                error = "a preserved event is invalid";
                return false;
            }
            const auto parsed = eventKindFromName(kind->asString());
            if (!parsed) {
                error = "a preserved event kind is unknown";
                return false;
            }
            event.kind = *parsed;
            event.sequence = sequence;
            if (const auto* track = item.find("sourceTrack"); track != nullptr && track->kind() == persist::Json::Kind::number) {
                event.sourceTrack = static_cast<int>(track->asNumber());
            }
            event.hasChannel = item.find("hasChannel") != nullptr && item.find("hasChannel")->asBool();
            readU8(item.find("channel"), event.channel);
            readU8(item.find("data1"), event.data1);
            readU8(item.find("data2"), event.data2);
            readU8(item.find("metaType"), event.metaType);
            event.affectsSound = item.find("affectsSound") != nullptr && item.find("affectsSound")->asBool();
            if (const auto* payload = item.find("payload"); payload != nullptr && payload->kind() == persist::Json::Kind::array) {
                for (const auto& byte : payload->asArray()) {
                    std::uint8_t value = 0;
                    if (!readU8(&byte, value)) {
                        error = "preserved payload is invalid";
                        return false;
                    }
                    event.payload.push_back(value);
                }
            }
            song.preserved.push_back(std::move(event));
            return true;
        })) {
        return std::nullopt;
    }
    if (!readObjects("diagnostics", [&](const persist::Json& item) {
            Diagnostic diagnostic;
            if (const auto* code = item.find("code")) diagnostic.code = code->asString();
            if (const auto* message = item.find("message")) diagnostic.message = message->asString();
            readU32(item.find("tick"), diagnostic.tick);
            if (const auto* track = item.find("track"); track != nullptr && track->kind() == persist::Json::Kind::number) {
                diagnostic.track = static_cast<int>(track->asNumber());
            }
            if (const auto* object = item.find("object")) diagnostic.objectId = object->asString();
            song.diagnostics.push_back(std::move(diagnostic));
            return true;
        })) {
        return std::nullopt;
    }
    if (!readObjects("appliedRequests", [&](const persist::Json& item) {
            if (item.kind() != persist::Json::Kind::string) {
                error = "applied request ids must be strings";
                return false;
            }
            song.appliedRequests.push_back(item.asString());
            return true;
        })) {
        return std::nullopt;
    }
    const auto readStack = [&](const char* key, std::vector<std::string>& stack) {
        return readObjects(key, [&](const persist::Json& item) {
            if (item.kind() != persist::Json::Kind::string) {
                error = "history entries must be strings";
                return false;
            }
            stack.push_back(item.asString());
            return true;
        });
    };
    if (!readStack("undo", song.undoStack) || !readStack("redo", song.redoStack)) return std::nullopt;
    bool allZeroOrder = true;
    for (const auto& track : song.tracks) {
        if (track.order != 0) allZeroOrder = false;
    }
    if (allZeroOrder) {
        for (int index = 0; index < static_cast<int>(song.tracks.size()); ++index) song.tracks[static_cast<std::size_t>(index)].order = index;
    }
    return song;
}

std::optional<SongDocument> loadSong(const std::filesystem::path& path, std::string& error) {
    std::string text;
    if (!readText(path, text, error)) return std::nullopt;
    auto json = persist::Json::parse(text, error);
    if (!json) return std::nullopt;
    return songFromJson(*json, error);
}

bool saveSong(const std::filesystem::path& path, const SongDocument& song, std::string& error) {
    if (path.empty()) {
        error = "missing song path";
        return false;
    }
    std::filesystem::path temporary = path;
    temporary += ".nodsynth-tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "failed to write the temporary song file";
            return false;
        }
        const auto text = toJson(song).dump();
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            error = "failed to write the temporary song file";
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    if (!replaceFile(temporary, path, error)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

Validation validate(const SongDocument& song) { return validateDocument(song); }

Validation validateDocument(const SongDocument& song) {
    Validation result;
    if (song.version != kSongFormatVersion) result.diagnostics.push_back({"version", "song version is not supported"});
    if (song.ppq == 0) result.diagnostics.push_back({"ppq", "ppq must be positive"});
    if (song.tempo.empty() || song.tempo.front().tick != 0) {
        result.diagnostics.push_back({"tempo", "tempo map must start at tick 0"});
    }
    for (std::size_t index = 1; index < song.tempo.size(); ++index) {
        if (song.tempo[index].tick < song.tempo[index - 1].tick || song.tempo[index].microsecondsPerQuarter == 0) {
            result.diagnostics.push_back({"tempo", "tempo points must be ordered and non-zero", song.tempo[index].tick});
        }
    }
    std::vector<std::string> ids;
    for (const auto& resource : song.resources) addUnique(ids, resource.id, "resource", result);
    for (const auto& instrument : song.instruments) {
        addUnique(ids, instrument.id, "instrument", result);
        if (!instrument.resourceId.empty() &&
            std::none_of(song.resources.begin(), song.resources.end(), [&](const Resource& resource) {
                return resource.id == instrument.resourceId;
            })) {
            result.diagnostics.push_back({"missing-resource", "instrument resource was not found", 0, -1, instrument.id});
        }
    }
    for (const auto& track : song.tracks) {
        addUnique(ids, track.id, "track", result);
        if (track.pan < -1.0 || track.pan > 1.0 || !std::isfinite(track.gain) || track.gain < 0.0) {
            result.diagnostics.push_back({"mix", "track gain or pan is invalid", 0, -1, track.id});
        }
        if (!track.instrumentId.empty() &&
            std::none_of(song.instruments.begin(), song.instruments.end(), [&](const Instrument& instrument) {
                return instrument.id == track.instrumentId;
            })) {
            result.diagnostics.push_back({"missing-instrument", "track instrument was not found", 0, -1, track.id});
        }
        std::uint32_t previousGain = 0;
        bool haveGain = false;
        for (const auto& point : track.gainAutomation) {
            if ((haveGain && point.tick < previousGain) || point.gain < 0.0 || !std::isfinite(point.gain)) {
                result.diagnostics.push_back({"automation", "gain automation is invalid", point.tick, -1, track.id});
            }
            previousGain = point.tick;
            haveGain = true;
        }
        for (const auto& clip : track.clips) {
            addUnique(ids, clip.id, "clip", result);
            for (const auto& note : clip.notes) {
                addUnique(ids, note.id, "note", result);
                if (note.channel > 15) result.diagnostics.push_back({"note", "note channel is invalid", note.tick, -1, note.id});
                const auto start = static_cast<std::uint64_t>(clip.startTick) + note.tick;
                if (start + note.duration > 0xffffffffu) {
                    result.diagnostics.push_back({"note", "note extends past the tick range", note.tick, -1, note.id});
                }
            }
        }
        for (const auto& event : track.performance) addUnique(ids, event.id, "performance", result);
    }
    result.ok = result.diagnostics.empty();
    return result;
}

Validation validateRenderReady(const SongDocument& song) {
    auto result = validateDocument(song);
    for (const auto& track : song.tracks) {
        bool hasMusic = !track.performance.empty();
        for (const auto& clip : track.clips) {
            if (!clip.notes.empty()) hasMusic = true;
        }
        if (hasMusic && track.instrumentId.empty()) {
            result.diagnostics.push_back({"missing-patch", "a track with music has no instrument", 0, -1, track.id});
            result.ok = false;
        }
    }
    return result;
}

persist::Json summaryJson(const SongDocument& song, const Validation& validation) {
    persist::Json json = persist::Json::object();
    json.set("status", persist::Json::string(validation.ok ? "ok" : "rejected"));
    json.set("revision", persist::Json::number(static_cast<double>(song.revision)));
    json.set("ppq", persist::Json::number(song.ppq));
    json.set("tracks", persist::Json::number(static_cast<double>(song.tracks.size())));
    persist::Json trackJson = persist::Json::array();
    for (const auto& track : song.tracks) {
        persist::Json item = persist::Json::object();
        item.set("id", persist::Json::string(track.id));
        item.set("name", persist::Json::string(track.name));
        item.set("instrument", persist::Json::string(track.instrumentId));
        item.set("gain", persist::Json::number(track.gain));
        item.set("pan", persist::Json::number(track.pan));
        std::uint32_t notes = 0;
        for (const auto& clip : track.clips) notes += static_cast<std::uint32_t>(clip.notes.size());
        item.set("notes", persist::Json::number(notes));
        trackJson.push(std::move(item));
    }
    json.set("trackList", std::move(trackJson));
    persist::Json diagnostics = persist::Json::array();
    for (const auto& diagnostic : validation.diagnostics) diagnostics.push(diagnosticJson(diagnostic));
    json.set("diagnostics", std::move(diagnostics));
    return json;
}

namespace {
struct ListedNote {
    std::string id;
    std::string track;
    std::uint32_t tick{0};
    std::uint32_t duration{0};
    std::uint8_t pitch{0};
    std::uint8_t velocity{0};
};

std::vector<ListedNote> listNotes(const SongDocument& song) {
    std::vector<ListedNote> notes;
    for (const auto& track : song.tracks) {
        for (const auto& clip : track.clips) {
            for (const auto& note : clip.notes) {
                const auto tick = static_cast<std::uint64_t>(clip.startTick) + note.tick;
                notes.push_back(
                    {note.id, track.id, static_cast<std::uint32_t>(std::min<std::uint64_t>(tick, 0xffffffffu)), note.duration, note.pitch,
                     note.velocity});
            }
        }
    }
    return notes;
}

persist::Json noteJson(const ListedNote& note) {
    persist::Json item = persist::Json::object();
    item.set("id", persist::Json::string(note.id));
    item.set("track", persist::Json::string(note.track));
    item.set("tick", persist::Json::number(note.tick));
    item.set("duration", persist::Json::number(note.duration));
    item.set("pitch", persist::Json::number(note.pitch));
    item.set("velocity", persist::Json::number(note.velocity));
    return item;
}

bool sameNote(const ListedNote& left, const ListedNote& right) {
    return left.track == right.track && left.tick == right.tick && left.duration == right.duration && left.pitch == right.pitch &&
           left.velocity == right.velocity;
}

std::string historyImage(const SongDocument& song) {
    SongDocument copy = song;
    copy.undoStack.clear();
    copy.redoStack.clear();
    return toJson(copy).dump(-1);
}

std::optional<SongDocument> songFromImage(const std::string& image, std::string& error) {
    auto json = persist::Json::parse(image, error);
    if (!json) return std::nullopt;
    return songFromJson(*json, error);
}
} // namespace

ApplyResult undoSong(SongDocument& song) {
    ApplyResult result;
    result.revision = song.revision;
    if (song.undoStack.empty()) {
        result.code = "nothing-to-undo";
        result.message = "there is no edit to undo";
        return result;
    }
    const auto image = song.undoStack.back();
    auto undo = song.undoStack;
    undo.pop_back();
    auto redo = song.redoStack;
    redo.push_back(historyImage(song));
    std::string error;
    auto restored = songFromImage(image, error);
    if (!restored) {
        result.code = "bad-history";
        result.message = error.empty() ? "the undo snapshot is invalid" : error;
        return result;
    }
    restored->undoStack = std::move(undo);
    restored->redoStack = std::move(redo);
    result.diff = semanticDiff(song, *restored, false);
    song = std::move(*restored);
    result.ok = true;
    result.baseRevision = result.revision;
    result.revision = song.revision;
    result.message = "undone";
    return result;
}

ApplyResult redoSong(SongDocument& song) {
    ApplyResult result;
    result.revision = song.revision;
    if (song.redoStack.empty()) {
        result.code = "nothing-to-redo";
        result.message = "there is no edit to redo";
        return result;
    }
    const auto image = song.redoStack.back();
    auto redo = song.redoStack;
    redo.pop_back();
    auto undo = song.undoStack;
    undo.push_back(historyImage(song));
    std::string error;
    auto restored = songFromImage(image, error);
    if (!restored) {
        result.code = "bad-history";
        result.message = error.empty() ? "the redo snapshot is invalid" : error;
        return result;
    }
    restored->undoStack = std::move(undo);
    restored->redoStack = std::move(redo);
    result.diff = semanticDiff(song, *restored, false);
    song = std::move(*restored);
    result.ok = true;
    result.baseRevision = result.revision;
    result.revision = song.revision;
    result.message = "redone";
    return result;
}

ApplyResult applyCommands(SongDocument& song, const persist::Json& batch, std::optional<std::uint64_t> expectRevision, bool dryRun) {
    ApplyResult result;
    result.revision = song.revision;
    result.baseRevision = song.revision;
    const auto* schema = batch.find("schemaVersion");
    std::uint32_t schemaVersion = 0;
    if (schema == nullptr || !readU32(schema, schemaVersion) || schemaVersion != 1) {
        result.code = "bad-command";
        result.message = "command schemaVersion must be 1";
        return result;
    }
    std::string requestId;
    if (const auto* request = batch.find("requestId"); request != nullptr && request->kind() == persist::Json::Kind::string) {
        requestId = request->asString();
    }
    result.requestId = requestId;
    if (!requestId.empty() &&
        std::find(song.appliedRequests.begin(), song.appliedRequests.end(), requestId) != song.appliedRequests.end()) {
        result.ok = true;
        result.unchanged = true;
        result.noop = true;
        result.message = "request already applied";
        result.diff = persist::Json::object();
        result.diff.set("changes", persist::Json::array());
        result.diff.set("added", persist::Json::array());
        result.diff.set("removed", persist::Json::array());
        result.diff.set("changedEntityCount", persist::Json::number(0));
        return result;
    }
    if (expectRevision && *expectRevision != song.revision) {
        result.code = "revision-conflict";
        result.message = "song revision does not match";
        result.revision = song.revision;
        return result;
    }
    const auto* commands = batch.find("commands");
    if (commands == nullptr || commands->kind() != persist::Json::Kind::array) {
        result.code = "bad-command";
        result.message = "commands must be an array";
        return result;
    }
    const auto image = historyImage(song);
    SongDocument next = song;
    persist::Json idMap = persist::Json::object();
    for (const auto& command : commands->asArray()) {
        std::string error;
        if (!applyOne(next, command, error, idMap)) {
            result.code = "bad-command";
            result.message = error;
            return result;
        }
    }
    const auto validation = validateDocument(next);
    if (!validation.ok && validateDocument(song).ok) {
        result.code = "invalid-song";
        result.message = validation.diagnostics.empty() ? "song failed validation" : validation.diagnostics.front().message;
        return result;
    }
    result.diff = semanticDiff(song, next, false);
    result.idMap = std::move(idMap);
    const auto* count = result.diff.find("changedEntityCount");
    const bool semanticChange = count != nullptr && count->asNumber() > 0;
    if (!semanticChange) {
        result.ok = true;
        result.unchanged = true;
        result.noop = true;
        result.message = "no semantic change";
        if (!requestId.empty() && !dryRun) {
            song.appliedRequests.push_back(requestId);
        }
        return result;
    }
    if (!requestId.empty()) next.appliedRequests.push_back(requestId);
    ++next.revision;
    next.undoStack.push_back(image);
    next.redoStack.clear();
    if (next.undoStack.size() > 32) next.undoStack.erase(next.undoStack.begin());
    result.ok = true;
    result.revision = next.revision;
    result.message = dryRun ? "previewed" : "applied";
    if (!dryRun) song = std::move(next);
    else result.revision = song.revision;
    return result;
}

bool bindPatch(
    SongDocument& song,
    const std::string& trackId,
    std::string storedPath,
    const std::filesystem::path& fileToHash,
    std::string& error) {
    auto* track = findTrack(song, trackId);
    if (track == nullptr) {
        error = "track was not found";
        return false;
    }
    if (storedPath.empty()) {
        error = "patch path is empty";
        return false;
    }
    Resource* resource = nullptr;
    for (auto& candidate : song.resources) {
        if (candidate.kind == "patch" && candidate.path == storedPath) resource = &candidate;
    }
    if (resource == nullptr) {
        Resource created;
        created.id = freshId(song, "res");
        created.path = storedPath;
        created.kind = "patch";
        song.resources.push_back(std::move(created));
        resource = &song.resources.back();
    }
    resource->hash = hashFile(fileToHash);
    Instrument* instrument = nullptr;
    if (!track->instrumentId.empty()) {
        for (auto& candidate : song.instruments) {
            if (candidate.id == track->instrumentId) instrument = &candidate;
        }
    }
    if (instrument == nullptr) {
        Instrument created;
        created.id = freshId(song, "inst");
        created.kind = InstrumentKind::nodsynth;
        created.name = track->name.empty() ? track->id : track->name;
        song.instruments.push_back(std::move(created));
        instrument = &song.instruments.back();
        track->instrumentId = instrument->id;
    }
    instrument->kind = InstrumentKind::nodsynth;
    instrument->adapter.clear();
    instrument->resourceId = resource->id;
    return true;
}

bool bindExternal(
    SongDocument& song,
    const std::string& trackId,
    std::string adapter,
    std::string storedPath,
    const std::filesystem::path& fileToHash,
    std::string& error) {
    auto* track = findTrack(song, trackId);
    if (track == nullptr) {
        error = "track was not found";
        return false;
    }
    if (adapter.empty() || storedPath.empty()) {
        error = "external adapter and asset path are required";
        return false;
    }
    Resource* resource = nullptr;
    for (auto& candidate : song.resources) {
        if (candidate.kind == "soundfont" && candidate.path == storedPath) resource = &candidate;
    }
    if (resource == nullptr) {
        Resource created;
        created.id = freshId(song, "res");
        created.path = std::move(storedPath);
        created.kind = "soundfont";
        song.resources.push_back(std::move(created));
        resource = &song.resources.back();
    }
    resource->hash = hashFile(fileToHash);
    Instrument created;
    created.id = freshId(song, "inst");
    created.kind = InstrumentKind::externalCli;
    created.adapter = std::move(adapter);
    created.resourceId = resource->id;
    created.name = track->name.empty() ? track->id : track->name;
    track->instrumentId = created.id;
    song.instruments.push_back(std::move(created));
    return true;
}
} // namespace nodsynth::song
