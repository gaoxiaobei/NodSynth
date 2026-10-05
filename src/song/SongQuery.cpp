#include <nodsynth/song/SongDocument.h>

#include <algorithm>
#include <cmath>
#include <sstream>

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

std::string fnv(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hex64(hash);
}

std::uint32_t ticksPerBar(const TimeSignaturePoint& signature, std::uint16_t ppq) {
    const auto den = signature.denominator == 0 ? 4 : signature.denominator;
    return static_cast<std::uint32_t>(signature.numerator) * (static_cast<std::uint32_t>(ppq) * 4u / den);
}

TimeSignaturePoint signatureAt(const SongDocument& song, std::uint32_t tick) {
    TimeSignaturePoint current = song.timeSignatures.empty() ? TimeSignaturePoint{} : song.timeSignatures.front();
    for (const auto& point : song.timeSignatures) {
        if (point.tick > tick) break;
        current = point;
    }
    return current;
}

std::uint32_t noteCount(const Track& track) {
    std::uint32_t count = 0;
    for (const auto& clip : track.clips) count += static_cast<std::uint32_t>(clip.notes.size());
    return count;
}

persist::Json jsonString(std::string text) { return persist::Json::string(std::move(text)); }
persist::Json jsonNumber(double value) { return persist::Json::number(value); }

persist::Json changeJson(
    const char* entityType, const std::string& id, const char* field, persist::Json before, persist::Json after) {
    persist::Json item = persist::Json::object();
    item.set("entityType", jsonString(entityType));
    item.set("id", jsonString(id));
    item.set("field", jsonString(field));
    item.set("before", std::move(before));
    item.set("after", std::move(after));
    return item;
}

persist::Json entityJson(const char* entityType, const std::string& id, persist::Json data) {
    persist::Json item = persist::Json::object();
    item.set("entityType", jsonString(entityType));
    item.set("id", jsonString(id));
    item.set("data", std::move(data));
    return item;
}

persist::Json noteData(const Note& note, const std::string& trackId, const std::string& clipId, std::uint32_t clipStart) {
    persist::Json item = persist::Json::object();
    item.set("track", jsonString(trackId));
    item.set("clip", jsonString(clipId));
    item.set("tick", jsonNumber(note.tick));
    item.set("absoluteTick", jsonNumber(static_cast<double>(clipStart) + note.tick));
    item.set("duration", jsonNumber(note.duration));
    item.set("pitch", jsonNumber(note.pitch));
    item.set("velocity", jsonNumber(note.velocity));
    item.set("channel", jsonNumber(note.channel));
    return item;
}

persist::Json automationSummary(const std::vector<GainPoint>& points) {
    persist::Json item = persist::Json::object();
    item.set("points", jsonNumber(static_cast<double>(points.size())));
    if (!points.empty()) {
        item.set("startTick", jsonNumber(points.front().tick));
        item.set("endTick", jsonNumber(points.back().tick));
    }
    std::ostringstream text;
    for (const auto& point : points) text << point.tick << ':' << point.gain << ';';
    item.set("hash", jsonString(fnv(text.str())));
    return item;
}

persist::Json automationFull(const std::vector<GainPoint>& points) {
    persist::Json list = persist::Json::array();
    for (const auto& point : points) {
        persist::Json item = persist::Json::object();
        item.set("tick", jsonNumber(point.tick));
        item.set("gain", jsonNumber(point.gain));
        list.push(std::move(item));
    }
    return list;
}

const Track* findTrack(const SongDocument& song, const std::string& id) {
    for (const auto& track : song.tracks) {
        if (track.id == id) return &track;
    }
    return nullptr;
}

std::vector<std::string> registeredCommands() {
    return {
        "set-instrument",
        "set-gain",
        "set-pan",
        "set-gain-automation",
        "set-parameter-automation",
        "move-clip",
        "move-note",
        "add-note",
        "delete-note",
        "create-track",
        "delete-track",
        "rename-track",
        "reorder-track",
        "create-clip",
        "delete-clip",
        "duplicate-clip",
        "add-notes",
        "update-note",
        "delete-notes",
        "set-tempo",
        "set-time-signature",
        "set-song-range",
        "bind-preset",
    };
}

persist::Json capabilitiesJson(const SongDocument& song) {
    persist::Json capabilities = persist::Json::object();
    persist::Json commands = persist::Json::array();
    for (const auto& command : registeredCommands()) commands.push(jsonString(command));
    capabilities.set("commands", std::move(commands));
    persist::Json midi = persist::Json::array();
    for (const char* event : {"note-on", "note-off", "pitch-bend", "control-change:64", "control-change:120", "control-change:123"}) {
        midi.push(jsonString(event));
    }
    capabilities.set("midi", std::move(midi));
    persist::Json automation = persist::Json::array();
    automation.push(jsonString("track-gain"));
    automation.push(jsonString("instrument-parameter"));
    capabilities.set("automation", std::move(automation));
    capabilities.set("undo", persist::Json::boolean(!song.undoStack.empty()));
    capabilities.set("redo", persist::Json::boolean(!song.redoStack.empty()));
    capabilities.set("schemaVersion", jsonNumber(1));
    capabilities.set("songFormatVersion", jsonNumber(kSongFormatVersion));
    persist::Json parameters = persist::Json::array();
    persist::Json gain = persist::Json::object();
    gain.set("id", jsonString("gain"));
    gain.set("unit", jsonString("linear-amplitude"));
    gain.set("minimum", jsonNumber(0));
    gain.set("automatable", persist::Json::boolean(true));
    parameters.push(std::move(gain));
    persist::Json pan = persist::Json::object();
    pan.set("id", jsonString("pan"));
    pan.set("unit", jsonString("equal-power"));
    pan.set("minimum", jsonNumber(-1));
    pan.set("maximum", jsonNumber(1));
    pan.set("automatable", persist::Json::boolean(false));
    parameters.push(std::move(pan));
    capabilities.set("parameters", std::move(parameters));
    return capabilities;
}

persist::Json trackSummary(const Track& track, int order) {
    persist::Json item = persist::Json::object();
    item.set("id", jsonString(track.id));
    item.set("name", jsonString(track.name));
    item.set("order", jsonNumber(order));
    item.set("instrument", jsonString(track.instrumentId));
    item.set("gain", jsonNumber(track.gain));
    item.set("pan", jsonNumber(track.pan));
    if (track.sourceTrack) item.set("sourceTrack", jsonNumber(*track.sourceTrack));
    if (track.sourceChannel) item.set("sourceChannel", jsonNumber(*track.sourceChannel));
    item.set("clips", jsonNumber(static_cast<double>(track.clips.size())));
    item.set("notes", jsonNumber(noteCount(track)));
    item.set("gainAutomationPoints", jsonNumber(static_cast<double>(track.gainAutomation.size())));
    persist::Json clips = persist::Json::array();
    for (const auto& clip : track.clips) {
        persist::Json clipJson = persist::Json::object();
        clipJson.set("id", jsonString(clip.id));
        clipJson.set("startTick", jsonNumber(clip.startTick));
        clipJson.set("length", jsonNumber(clip.length));
        clipJson.set("notes", jsonNumber(static_cast<double>(clip.notes.size())));
        clips.push(std::move(clipJson));
    }
    item.set("clipList", std::move(clips));
    item.set("gainAutomation", automationSummary(track.gainAutomation));
    return item;
}

std::vector<const Track*> orderedTracks(const SongDocument& song) {
    std::vector<const Track*> tracks;
    tracks.reserve(song.tracks.size());
    for (const auto& track : song.tracks) tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(), [](const Track* left, const Track* right) {
        if (left->order != right->order) return left->order < right->order;
        return left->id < right->id;
    });
    return tracks;
}

struct IndexedNote {
    std::string id;
    std::string track;
    std::string clip;
    std::uint32_t tick{0};
    std::uint32_t duration{0};
    std::uint8_t pitch{0};
    std::uint8_t velocity{0};
    std::uint32_t clipStart{0};
};

std::vector<IndexedNote> collectNotes(const SongDocument& song) {
    std::vector<IndexedNote> notes;
    for (const auto& track : song.tracks) {
        for (const auto& clip : track.clips) {
            for (const auto& note : clip.notes) {
                IndexedNote item;
                item.id = note.id;
                item.track = track.id;
                item.clip = clip.id;
                item.clipStart = clip.startTick;
                item.tick = note.tick;
                item.duration = note.duration;
                item.pitch = note.pitch;
                item.velocity = note.velocity;
                notes.push_back(std::move(item));
            }
        }
    }
    std::stable_sort(notes.begin(), notes.end(), [](const IndexedNote& left, const IndexedNote& right) {
        const auto leftAbs = static_cast<std::uint64_t>(left.clipStart) + left.tick;
        const auto rightAbs = static_cast<std::uint64_t>(right.clipStart) + right.tick;
        if (leftAbs != rightAbs) return leftAbs < rightAbs;
        return left.id < right.id;
    });
    return notes;
}

bool decodeCursor(const std::string& cursor, std::uint64_t revision, std::size_t& offset, std::string& error) {
    const auto colon = cursor.find(':');
    if (colon == std::string::npos) {
        error = "cursor is invalid";
        return false;
    }
    try {
        const auto cursorRevision = std::stoull(cursor.substr(0, colon));
        offset = static_cast<std::size_t>(std::stoull(cursor.substr(colon + 1)));
        if (cursorRevision != revision) {
            error = "cursor is stale";
            return false;
        }
    } catch (...) {
        error = "cursor is invalid";
        return false;
    }
    return true;
}

persist::Json tempoOverview(const SongDocument& song) {
    persist::Json json = persist::Json::object();
    json.set("ppq", jsonNumber(song.ppq));
    json.set("endTick", jsonNumber(endTick(song)));
    if (song.songRangeEndTick) json.set("songRangeEndTick", jsonNumber(*song.songRangeEndTick));
    if (!song.tempo.empty()) {
        const double bpm = 60000000.0 / static_cast<double>(song.tempo.front().microsecondsPerQuarter);
        json.set("bpm", jsonNumber(bpm));
    }
    if (!song.timeSignatures.empty()) {
        json.set("numerator", jsonNumber(song.timeSignatures.front().numerator));
        json.set("denominator", jsonNumber(song.timeSignatures.front().denominator));
    }
    json.set("tempoPoints", jsonNumber(static_cast<double>(song.tempo.size())));
    json.set("tracks", jsonNumber(static_cast<double>(song.tracks.size())));
    std::uint32_t clips = 0;
    std::uint32_t notes = 0;
    for (const auto& track : song.tracks) {
        clips += static_cast<std::uint32_t>(track.clips.size());
        notes += noteCount(track);
    }
    json.set("clips", jsonNumber(clips));
    json.set("notes", jsonNumber(notes));
    json.set("revision", jsonNumber(static_cast<double>(song.revision)));
    persist::Json diagnostics = persist::Json::array();
    for (const auto& diagnostic : song.diagnostics) {
        persist::Json item = persist::Json::object();
        item.set("code", jsonString(diagnostic.code));
        item.set("message", jsonString(diagnostic.message));
        diagnostics.push(std::move(item));
    }
    json.set("diagnostics", std::move(diagnostics));
    return json;
}
} // namespace

SongDocument createSong(const CreateSongOptions& options) {
    SongDocument song;
    song.version = kSongFormatVersion;
    song.revision = 1;
    song.ppq = options.ppq == 0 ? 480 : options.ppq;
    const auto bpm = options.bpm > 0 ? options.bpm : 120.0;
    TempoPoint tempo;
    tempo.tick = 0;
    tempo.microsecondsPerQuarter = static_cast<std::uint32_t>(std::llround(60000000.0 / bpm));
    if (tempo.microsecondsPerQuarter == 0) tempo.microsecondsPerQuarter = midi::kDefaultTempoUs;
    song.tempo.push_back(tempo);
    TimeSignaturePoint meter;
    meter.tick = 0;
    meter.numerator = options.numerator == 0 ? 4 : options.numerator;
    meter.denominator = options.denominator == 0 ? 4 : options.denominator;
    song.timeSignatures.push_back(meter);
    const auto bars = options.bars == 0 ? 16u : options.bars;
    song.songRangeEndTick = ticksPerBar(meter, song.ppq) * bars;
    return song;
}

bool barsToTicks(
    const SongDocument& song, std::uint32_t startBar, std::uint32_t endBar, std::uint32_t& startTick, std::uint32_t& endTickOut, std::string& error) {
    if (startBar == 0 || endBar <= startBar) {
        error = "bars are 1-based and the end bar must be after the start bar";
        return false;
    }
    std::uint32_t tick = 0;
    startTick = 0;
    for (std::uint32_t bar = 1; bar < endBar; ++bar) {
        if (bar == startBar) startTick = tick;
        const auto span = ticksPerBar(signatureAt(song, tick), song.ppq);
        if (span == 0 || tick > 0xffffffffu - span) {
            error = "bar range does not fit in the song";
            return false;
        }
        tick += span;
    }
    endTickOut = tick;
    return true;
}

persist::Json semanticDiff(const SongDocument& before, const SongDocument& after, bool expandAutomation) {
    persist::Json diff = persist::Json::object();
    persist::Json changes = persist::Json::array();
    persist::Json added = persist::Json::array();
    persist::Json removed = persist::Json::array();
    persist::Json tracksTouched = persist::Json::array();
    std::vector<std::string> trackIds;
    const auto touch = [&](const std::string& id) {
        if (std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end()) {
            trackIds.push_back(id);
            tracksTouched.push(jsonString(id));
        }
    };

    if (before.ppq != after.ppq) {
        changes.push(changeJson("song", "", "ppq", jsonNumber(before.ppq), jsonNumber(after.ppq)));
    }
    const auto beforeRange = before.songRangeEndTick ? jsonNumber(*before.songRangeEndTick) : persist::Json::null();
    const auto afterRange = after.songRangeEndTick ? jsonNumber(*after.songRangeEndTick) : persist::Json::null();
    if (before.songRangeEndTick != after.songRangeEndTick) {
        persist::Json item = persist::Json::object();
        item.set("entityType", jsonString("song"));
        item.set("id", jsonString(""));
        item.set("field", jsonString("songRangeEndTick"));
        item.set("before", beforeRange);
        item.set("after", afterRange);
        changes.push(std::move(item));
    }
    if (before.tempo.size() != after.tempo.size() ||
        (!before.tempo.empty() && !after.tempo.empty() &&
         (before.tempo.front().microsecondsPerQuarter != after.tempo.front().microsecondsPerQuarter))) {
        const auto bpm = [](const SongDocument& song) {
            return song.tempo.empty() ? 0.0 : 60000000.0 / static_cast<double>(song.tempo.front().microsecondsPerQuarter);
        };
        changes.push(changeJson("song", "", "tempo", jsonNumber(bpm(before)), jsonNumber(bpm(after))));
    }
    if (before.timeSignatures.size() != after.timeSignatures.size() ||
        (!before.timeSignatures.empty() && !after.timeSignatures.empty() &&
         (before.timeSignatures.front().numerator != after.timeSignatures.front().numerator ||
          before.timeSignatures.front().denominator != after.timeSignatures.front().denominator))) {
        changes.push(changeJson(
            "song", "", "timeSignature",
            jsonString(std::to_string(before.timeSignatures.empty() ? 4 : before.timeSignatures.front().numerator) + "/" +
                       std::to_string(before.timeSignatures.empty() ? 4 : before.timeSignatures.front().denominator)),
            jsonString(std::to_string(after.timeSignatures.empty() ? 4 : after.timeSignatures.front().numerator) + "/" +
                       std::to_string(after.timeSignatures.empty() ? 4 : after.timeSignatures.front().denominator))));
    }

    for (const auto& track : after.tracks) {
        const auto* previous = findTrack(before, track.id);
        if (previous == nullptr) {
            persist::Json data = persist::Json::object();
            data.set("name", jsonString(track.name));
            data.set("order", jsonNumber(track.order));
            added.push(entityJson("track", track.id, std::move(data)));
            touch(track.id);
            continue;
        }
        if (previous->name != track.name) {
            changes.push(changeJson("track", track.id, "name", jsonString(previous->name), jsonString(track.name)));
            touch(track.id);
        }
        if (previous->order != track.order) {
            changes.push(changeJson("track", track.id, "order", jsonNumber(previous->order), jsonNumber(track.order)));
            touch(track.id);
        }
        if (previous->gain != track.gain) {
            changes.push(changeJson("track", track.id, "gain", jsonNumber(previous->gain), jsonNumber(track.gain)));
            touch(track.id);
        }
        if (previous->pan != track.pan) {
            changes.push(changeJson("track", track.id, "pan", jsonNumber(previous->pan), jsonNumber(track.pan)));
            touch(track.id);
        }
        if (previous->instrumentId != track.instrumentId) {
            changes.push(changeJson("track", track.id, "instrument", jsonString(previous->instrumentId), jsonString(track.instrumentId)));
            touch(track.id);
        }
        const auto beforeAuto = expandAutomation ? automationFull(previous->gainAutomation) : automationSummary(previous->gainAutomation);
        const auto afterAuto = expandAutomation ? automationFull(track.gainAutomation) : automationSummary(track.gainAutomation);
        if (beforeAuto.dump(-1) != afterAuto.dump(-1)) {
            persist::Json item = persist::Json::object();
            item.set("entityType", jsonString("track"));
            item.set("id", jsonString(track.id));
            item.set("field", jsonString("gainAutomation"));
            item.set("before", beforeAuto);
            item.set("after", afterAuto);
            changes.push(std::move(item));
            touch(track.id);
        }
        if (previous->parameterAutomation.size() != track.parameterAutomation.size()) {
            changes.push(changeJson(
                "track", track.id, "parameterAutomation", jsonNumber(static_cast<double>(previous->parameterAutomation.size())),
                jsonNumber(static_cast<double>(track.parameterAutomation.size()))));
            touch(track.id);
        }

        for (const auto& clip : track.clips) {
            const Clip* previousClip = nullptr;
            for (const auto& candidate : previous->clips) {
                if (candidate.id == clip.id) previousClip = &candidate;
            }
            if (previousClip == nullptr) {
                persist::Json data = persist::Json::object();
                data.set("track", jsonString(track.id));
                data.set("startTick", jsonNumber(clip.startTick));
                data.set("length", jsonNumber(clip.length));
                added.push(entityJson("clip", clip.id, std::move(data)));
                touch(track.id);
            } else if (previousClip->startTick != clip.startTick || previousClip->length != clip.length) {
                if (previousClip->startTick != clip.startTick) {
                    changes.push(changeJson("clip", clip.id, "startTick", jsonNumber(previousClip->startTick), jsonNumber(clip.startTick)));
                }
                if (previousClip->length != clip.length) {
                    changes.push(changeJson("clip", clip.id, "length", jsonNumber(previousClip->length), jsonNumber(clip.length)));
                }
                touch(track.id);
            }
            for (const auto& note : clip.notes) {
                const Note* previousNote = nullptr;
                if (previousClip != nullptr) {
                    for (const auto& candidate : previousClip->notes) {
                        if (candidate.id == note.id) previousNote = &candidate;
                    }
                }
                if (previousNote == nullptr) {
                    added.push(entityJson("note", note.id, noteData(note, track.id, clip.id, clip.startTick)));
                    touch(track.id);
                } else if (
                    previousNote->tick != note.tick || previousNote->duration != note.duration || previousNote->pitch != note.pitch ||
                    previousNote->velocity != note.velocity) {
                    persist::Json item = persist::Json::object();
                    item.set("entityType", jsonString("note"));
                    item.set("id", jsonString(note.id));
                    item.set("track", jsonString(track.id));
                    persist::Json fields = persist::Json::object();
                    if (previousNote->tick != note.tick) {
                        fields.set("tick", changeJson("note", note.id, "tick", jsonNumber(previousNote->tick), jsonNumber(note.tick)));
                    }
                    if (previousNote->pitch != note.pitch) {
                        changes.push(changeJson("note", note.id, "pitch", jsonNumber(previousNote->pitch), jsonNumber(note.pitch)));
                    }
                    if (previousNote->tick != note.tick) {
                        changes.push(changeJson("note", note.id, "tick", jsonNumber(previousNote->tick), jsonNumber(note.tick)));
                    }
                    if (previousNote->duration != note.duration) {
                        changes.push(changeJson("note", note.id, "duration", jsonNumber(previousNote->duration), jsonNumber(note.duration)));
                    }
                    if (previousNote->velocity != note.velocity) {
                        changes.push(changeJson("note", note.id, "velocity", jsonNumber(previousNote->velocity), jsonNumber(note.velocity)));
                    }
                    touch(track.id);
                    (void)item;
                    (void)fields;
                }
            }
            if (previousClip != nullptr) {
                for (const auto& note : previousClip->notes) {
                    bool found = false;
                    for (const auto& candidate : clip.notes) {
                        if (candidate.id == note.id) found = true;
                    }
                    if (!found) {
                        removed.push(entityJson("note", note.id, noteData(note, track.id, clip.id, previousClip->startTick)));
                        touch(track.id);
                    }
                }
            }
        }
        for (const auto& clip : previous->clips) {
            bool found = false;
            for (const auto& candidate : track.clips) {
                if (candidate.id == clip.id) found = true;
            }
            if (!found) {
                persist::Json data = persist::Json::object();
                data.set("track", jsonString(track.id));
                removed.push(entityJson("clip", clip.id, std::move(data)));
                touch(track.id);
            }
        }
    }
    for (const auto& track : before.tracks) {
        if (findTrack(after, track.id) == nullptr) {
            persist::Json data = persist::Json::object();
            data.set("name", jsonString(track.name));
            removed.push(entityJson("track", track.id, std::move(data)));
            touch(track.id);
        }
    }

    const auto changedEntityCount = changes.asArray().size() + added.asArray().size() + removed.asArray().size();
    persist::Json range = persist::Json::object();
    range.set("startTick", jsonNumber(0));
    range.set("endTick", jsonNumber(std::max(endTick(before), endTick(after))));
    diff.set("changes", std::move(changes));
    diff.set("added", std::move(added));
    diff.set("removed", std::move(removed));
    diff.set("changedEntityCount", jsonNumber(static_cast<double>(changedEntityCount)));
    diff.set("tracks", std::move(tracksTouched));
    diff.set("invalidateRange", std::move(range));
    return diff;
}

persist::Json querySong(const SongDocument& song, std::optional<std::uint32_t> startTick, std::optional<std::uint32_t> endTick) {
    QueryOptions options;
    options.view = "legacy";
    options.startTick = startTick;
    options.endTick = endTick;
    return querySong(song, options);
}

persist::Json querySong(const SongDocument& song, const QueryOptions& options) {
    persist::Json json = persist::Json::object();
    json.set("revision", jsonNumber(static_cast<double>(song.revision)));
    json.set("ppq", jsonNumber(song.ppq));
    json.set("view", jsonString(options.view));
    const auto view = options.view.empty() ? std::string("legacy") : options.view;
    if (view == "summary") {
        json.set("summary", tempoOverview(song));
        return json;
    }
    if (view == "capabilities") {
        json.set("capabilities", capabilitiesJson(song));
        return json;
    }
    if (view == "tracks") {
        persist::Json tracks = persist::Json::array();
        int order = 0;
        for (const auto* track : orderedTracks(song)) {
            if (options.trackId && *options.trackId != track->id) continue;
            tracks.push(trackSummary(*track, track->order != 0 ? track->order : order));
            ++order;
        }
        json.set("tracks", std::move(tracks));
        json.set("note", jsonString("Song track IDs are stable. --map TRACK:CHANNEL selects the MIDI source stream, not the Song track id."));
        return json;
    }
    if (view == "notes" || view == "automation") {
        if (view == "automation") {
            persist::Json lanes = persist::Json::array();
            for (const auto* track : orderedTracks(song)) {
                if (options.trackId && *options.trackId != track->id) continue;
                persist::Json item = persist::Json::object();
                item.set("track", jsonString(track->id));
                item.set("gainAutomation", options.expandAutomation ? automationFull(track->gainAutomation) : automationSummary(track->gainAutomation));
                lanes.push(std::move(item));
            }
            json.set("automation", std::move(lanes));
            json.set("total", jsonNumber(static_cast<double>(json.find("automation")->asArray().size())));
            json.set("hasMore", persist::Json::boolean(false));
            return json;
        }
        auto notes = collectNotes(song);
        persist::Json list = persist::Json::array();
        std::uint32_t total = 0;
        const auto start = options.startTick;
        const auto end = options.endTick;
        for (const auto& note : notes) {
            if (options.trackId && *options.trackId != note.track) continue;
            const auto absTick = static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(note.clipStart) + note.tick, 0xffffffffu));
            const auto absEnd = absTick + note.duration;
            if (start && (options.onsetOnly ? absTick < *start : absEnd <= *start)) continue;
            if (end && absTick >= *end) continue;
            ++total;
            persist::Json item = persist::Json::object();
            item.set("id", jsonString(note.id));
            item.set("track", jsonString(note.track));
            item.set("clip", jsonString(note.clip));
            item.set("tick", jsonNumber(absTick));
            item.set("clipTick", jsonNumber(note.tick));
            item.set("duration", jsonNumber(note.duration));
            item.set("pitch", jsonNumber(note.pitch));
            item.set("velocity", jsonNumber(note.velocity));
            list.push(std::move(item));
        }
        std::size_t offset = 0;
        if (options.cursor) {
            std::string error;
            if (!decodeCursor(*options.cursor, song.revision, offset, error)) {
                json.set("status", jsonString("rejected"));
                json.set("code", jsonString("stale-cursor"));
                json.set("message", jsonString(error));
                return json;
            }
        }
        persist::Json page = persist::Json::array();
        const auto limit = options.limit.value_or(static_cast<std::uint32_t>(list.asArray().size()));
        for (std::size_t index = offset; index < list.asArray().size() && page.asArray().size() < limit; ++index) {
            page.push(list.asArray()[index]);
        }
        const auto next = offset + page.asArray().size();
        json.set("notes", std::move(page));
        json.set("total", jsonNumber(total));
        json.set("hasMore", persist::Json::boolean(next < list.asArray().size()));
        if (next < list.asArray().size()) json.set("nextCursor", jsonString(std::to_string(song.revision) + ":" + std::to_string(next)));
        json.set("interval", jsonString("half-open"));
        json.set("onsetOnly", persist::Json::boolean(options.onsetOnly));
        return json;
    }

    json.set("capabilities", capabilitiesJson(song));
    persist::Json tracks = persist::Json::array();
    for (const auto& track : song.tracks) {
        persist::Json item = persist::Json::object();
        item.set("id", jsonString(track.id));
        item.set("name", jsonString(track.name));
        item.set("order", jsonNumber(track.order));
        if (track.sourceTrack) item.set("sourceTrack", jsonNumber(*track.sourceTrack));
        if (track.sourceChannel) item.set("sourceChannel", jsonNumber(*track.sourceChannel));
        if (!track.clips.empty()) item.set("clip", jsonString(track.clips.front().id));
        tracks.push(std::move(item));
    }
    json.set("tracks", std::move(tracks));
    persist::Json notes = persist::Json::array();
    for (const auto& note : collectNotes(song)) {
        const auto absTick = static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(note.clipStart) + note.tick, 0xffffffffu));
        if (options.startTick && absTick < *options.startTick) continue;
        if (options.endTick && absTick >= *options.endTick) continue;
        persist::Json item = persist::Json::object();
        item.set("id", jsonString(note.id));
        item.set("track", jsonString(note.track));
        item.set("tick", jsonNumber(absTick));
        item.set("duration", jsonNumber(note.duration));
        item.set("pitch", jsonNumber(note.pitch));
        item.set("velocity", jsonNumber(note.velocity));
        notes.push(std::move(item));
    }
    json.set("notes", std::move(notes));
    return json;
}
} // namespace nodsynth::song
