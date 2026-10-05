#include <nodsynth/song/SongDocument.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <nodsynth/song/Automation.h>
#include <nodsynth/render/OfflineRenderer.h>

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
    const auto numerator = static_cast<std::uint64_t>(signature.numerator) * ppq * 4u;
    if (numerator % den || numerator / den > UINT32_MAX) return 0;
    return static_cast<std::uint32_t>(numerator / den);
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
        "set-parameter",
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
        "collect-resources",
        "add-pattern",
        "transpose-notes",
        "scale-velocities",
        "add-pump",
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
    auto instrumentParameters = persist::Json::array();
    for (const auto& track : song.tracks) {
        for (const auto& instrument : song.instruments) {
            if (instrument.id != track.instrumentId || instrument.kind != InstrumentKind::nodsynth) continue;
            for (const auto& resource : song.resources) {
                if (resource.id != instrument.resourceId) continue;
                std::string error;
                auto graph = render::loadPatch(resolveResourcePath(song.baseDirectory, resource.path), error);
                auto item = persist::Json::object(); item.set("track", jsonString(track.id));
                item.set("backend", jsonString("nodsynth"));
                if (graph) item.set("parameters", parametersJson(*graph));
                else item.set("error", jsonString(error));
                instrumentParameters.push(std::move(item));
            }
        }
    }
    capabilities.set("instrumentParameters", std::move(instrumentParameters));
    auto backends = persist::Json::array();
    for (const auto* backend : {"nodsynth", "vst3", "external-cli"}) {
        auto item = persist::Json::object(); item.set("backend", jsonString(backend));
        item.set("automatable", persist::Json::boolean(std::string(backend) != "external-cli"));
        item.set("address", jsonString(std::string(backend) == "vst3" ? "vst3:decimalParamId (legacy unambiguous titles accepted)" : "nodeId/parameterId or macro:id"));
        item.set("valueDomain", jsonString(std::string(backend) == "vst3" ? "normalized 0..1" : "physical (macros normalized)"));
        backends.push(std::move(item));
    }
    capabilities.set("backendParameters", std::move(backends));
    capabilities.set("interpolation", jsonString("NodSynth: step/linear, base before first point, hold after last, no additional offline smoothing; VST3: normalized linear"));
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
    auto parameters = persist::Json::array();
    for (const auto& lane : track.parameterAutomation) {
        auto item = persist::Json::object(); item.set("id", jsonString(lane.id));
        item.set("valueDomain", jsonString(lane.valueDomain)); item.set("interpolation", jsonString(lane.interpolation));
        item.set("points", jsonNumber(static_cast<double>(lane.points.size())));
        if (!lane.points.empty()) { item.set("startTick", jsonNumber(lane.points.front().tick)); item.set("endTick", jsonNumber(lane.points.back().tick)); }
        parameters.push(std::move(item));
    }
    item.set("parameterAutomation", std::move(parameters));
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
            error = "bar range cannot be represented exactly at this PPQ or exceeds the tick range";
            return false;
        }
        for (const auto& point : song.timeSignatures) {
            if (point.tick > tick && point.tick < tick + span) {
                error = "time-signature changes must occur on a bar boundary"; return false;
            }
        }
        tick += span;
    }
    endTickOut = tick;
    return true;
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
                auto parameters = persist::Json::array();
                for (const auto& lane : track->parameterAutomation) {
                    auto record = persist::Json::object(); record.set("id", jsonString(lane.id));
                    record.set("valueDomain", jsonString(lane.valueDomain)); record.set("interpolation", jsonString(lane.interpolation));
                    auto points = persist::Json::array();
                    for (const auto& point : lane.points) {
                        auto value = persist::Json::object(); value.set("tick", jsonNumber(point.tick)); value.set("value", jsonNumber(point.value)); points.push(std::move(value));
                    }
                    record.set("pointCount", jsonNumber(static_cast<double>(lane.points.size())));
                    record.set("hash", jsonString(fnv(points.dump(-1))));
                    if (options.expandAutomation) record.set("points", std::move(points));
                    parameters.push(std::move(record));
                }
                item.set("parameterAutomation", std::move(parameters));
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
