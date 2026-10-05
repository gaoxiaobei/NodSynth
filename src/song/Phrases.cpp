#include <nodsynth/song/Phrases.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nodsynth::song {
namespace {
using persist::Json;
bool integer(const Json* value, std::int64_t minimum, std::int64_t maximum, std::int64_t& out) {
    if (!value || value->kind() != Json::Kind::number || !std::isfinite(value->asNumber()) ||
        value->asNumber() < static_cast<double>(minimum) || value->asNumber() > static_cast<double>(maximum) ||
        std::floor(value->asNumber()) != value->asNumber()) return false;
    out = static_cast<std::int64_t>(value->asNumber()); return true;
}
bool fraction(const Json* value, std::uint16_t ppq, std::uint32_t& ticks, bool allowZero, std::string& error) {
    if (!value || value->kind() != Json::Kind::string) { error = "note values must be exact fractions such as 1/4"; return false; }
    const auto& text = value->asString(); const auto slash = text.find('/');
    if (slash == std::string::npos || text.find_first_not_of("0123456789/") != std::string::npos || text.find('/', slash + 1) != std::string::npos) {
        error = "invalid note-value fraction"; return false;
    }
    try {
        const auto numerator = std::stoull(text.substr(0, slash)), denominator = std::stoull(text.substr(slash + 1));
        if (!denominator || denominator > UINT32_MAX || numerator > UINT32_MAX || numerator * ppq * 4ull > UINT32_MAX * denominator ||
            numerator * ppq * 4ull % denominator || (!allowZero && !numerator)) {
            error = "note value cannot be represented exactly at this PPQ"; return false;
        }
        ticks = static_cast<std::uint32_t>(numerator * ppq * 4ull / denominator); return true;
    } catch (...) { error = "invalid note-value fraction"; return false; }
}
bool range(const SongDocument& song, const Json& command, std::uint32_t& from, std::uint32_t& to, std::string& error) {
    std::int64_t start = 0, end = 0;
    if (!integer(command.find("startBar"), 1, UINT32_MAX - 1, start) || !integer(command.find("endBar"), 2, UINT32_MAX, end)) {
        error = "phrase requires 1-based startBar and exclusive endBar"; return false;
    }
    return barsToTicks(song, static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(end), from, to, error);
}
bool chord(const Json& item, std::vector<int>& pitches, std::string& error) {
    std::int64_t root = 0;
    if (!integer(item.find("root"), 0, 127, root)) { error = "chord requires root MIDI pitch"; return false; }
    const auto* quality = item.find("quality");
    if (!quality || (quality->asString() != "major" && quality->asString() != "minor")) { error = "chord quality must be major or minor"; return false; }
    if (const auto* octave = item.find("octave")) {
        std::int64_t number = 0;
        if (!integer(octave, -1, 9, number)) { error = "invalid MIDI octave"; return false; }
        root = (number + 1) * 12 + root % 12;
    }
    pitches = {static_cast<int>(root), static_cast<int>(root) + (quality->asString() == "minor" ? 3 : 4), static_cast<int>(root) + 7};
    std::int64_t inversion = 0;
    if (item.find("inversion") && !integer(item.find("inversion"), 0, 2, inversion)) { error = "inversion must be 0, 1 or 2"; return false; }
    for (std::int64_t i = 0; i < inversion; ++i) { pitches.front() += 12; std::rotate(pitches.begin(), pitches.begin() + 1, pitches.end()); }
    if (const auto* voicing = item.find("voicing")) {
        if (voicing->kind() != Json::Kind::array || voicing->asArray().size() != 3) { error = "voicing requires three octave offsets"; return false; }
        for (std::size_t i = 0; i < 3; ++i) {
            std::int64_t offset = 0;
            if (!integer(&voicing->asArray()[i], -8, 8, offset)) { error = "invalid voicing octave offset"; return false; }
            pitches[i] += static_cast<int>(offset) * 12;
        }
    }
    return true;
}
bool pitchesAt(const Json& item, std::vector<int>& pitches, std::string& error) {
    pitches.clear();
    if (item.isNull()) return true;
    if (item.kind() == Json::Kind::object) return chord(item, pitches, error);
    if (item.kind() == Json::Kind::array) {
        for (const auto& value : item.asArray()) {
            std::int64_t number = 0;
            if (!integer(&value, 0, 127, number)) { error = "invalid chord pitch"; return false; }
            pitches.push_back(static_cast<int>(number));
        }
        return true;
    }
    std::int64_t pitch = 0;
    if (!integer(&item, 0, 127, pitch)) { error = "invalid pattern pitch"; return false; }
    pitches.push_back(static_cast<int>(pitch)); return true;
}
} // namespace

bool applyPhrase(SongDocument& song, const persist::Json& command, persist::Json& idMap, std::string& error) {
    const auto name = command.find("op")->asString();
    const auto* trackId = command.find("track");
    auto track = std::find_if(song.tracks.begin(), song.tracks.end(), [&](const auto& item) { return trackId && item.id == trackId->asString(); });
    if (track == song.tracks.end()) { error = "phrase track was not found"; return false; }
    if (name == "transpose-notes" || name == "scale-velocities") {
        if (command.find("clip") && std::none_of(track->clips.begin(), track->clips.end(), [&](const auto& clip) { return clip.id == command.find("clip")->asString(); })) {
            error = "selected clip was not found"; return false;
        }
        std::uint32_t from = 0, to = UINT32_MAX;
        if ((command.find("startBar") || command.find("endBar")) && !range(song, command, from, to, error)) return false;
        std::int64_t semitones = 0; double factor = 1;
        if (name == "transpose-notes" && !integer(command.find("semitones"), -127, 127, semitones)) { error = "invalid semitone count"; return false; }
        if (name == "scale-velocities") {
            const auto* value = command.find("factor");
            if (!value || value->kind() != Json::Kind::number || !std::isfinite(value->asNumber()) || value->asNumber() < 0 || value->asNumber() > 127) {
                error = "invalid velocity factor"; return false;
            }
            factor = value->asNumber();
        }
        for (auto& clip : track->clips) {
            if (command.find("clip") && command.find("clip")->asString() != clip.id) continue;
            for (auto& note : clip.notes) {
                const auto tick = static_cast<std::uint64_t>(clip.startTick) + note.tick;
                if (tick < from || tick >= to) continue;
                const auto value = name == "transpose-notes" ? static_cast<std::int64_t>(note.pitch) + semitones : std::llround(note.velocity * factor);
                const auto minimum = name == "transpose-notes" ? 0 : 1;
                if ((value < minimum || value > 127) && !(command.find("clamp") && command.find("clamp")->asBool())) {
                    error = "transformed note exceeds MIDI range; choose explicit clamp"; return false;
                }
                if (name == "transpose-notes") note.pitch = static_cast<std::uint8_t>(std::clamp<std::int64_t>(value, minimum, 127));
                else note.velocity = static_cast<std::uint8_t>(std::clamp<std::int64_t>(value, minimum, 127));
            }
        }
        return true;
    }
    std::uint32_t from = 0, to = 0;
    if (!range(song, command, from, to, error)) return false;
    if (name == "add-pump") {
        std::uint32_t period = 0, recovery = 0;
        if (!fraction(command.find("period"), song.ppq, period, false, error) || !fraction(command.find("recovery"), song.ppq, recovery, false, error)) return false;
        const auto* depth = command.find("depth");
        if (!depth || depth->kind() != Json::Kind::number || !std::isfinite(depth->asNumber()) || depth->asNumber() < 0 || depth->asNumber() > 1 || recovery >= period) {
            error = "pump requires depth 0..1 and recovery shorter than period"; return false;
        }
        std::vector<GainPoint> points;
        if ((to - from) / period > 100000) { error = "pump exceeds point limit"; return false; }
        for (std::uint64_t tick = from; tick < to; tick += period) {
            points.push_back({static_cast<std::uint32_t>(tick), 1 - depth->asNumber()});
            if (tick + recovery < to) points.push_back({static_cast<std::uint32_t>(tick + recovery), 1});
            if (tick + period - 1 < to) points.push_back({static_cast<std::uint32_t>(tick + period - 1), 1});
        }
        points.push_back({to, 1}); track->gainAutomation = std::move(points); return true;
    }
    const auto* id = command.find("id");
    if (!id || id->asString().empty()) { error = "pattern requires a stable clip id"; return false; }
    for (const auto& existing : song.tracks) for (const auto& clip : existing.clips)
        if (clip.id == id->asString()) { error = "pattern clip id already exists"; return false; }
    std::uint32_t grid = 0, duration = 0, offset = 0;
    if (!fraction(command.find("grid"), song.ppq, grid, false, error) || !fraction(command.find("duration"), song.ppq, duration, false, error)) return false;
    if (command.find("offset") && !fraction(command.find("offset"), song.ppq, offset, true, error)) return false;
    const auto* sequence = command.find("pitches"); const auto* velocities = command.find("velocities");
    if (!sequence || sequence->kind() != Json::Kind::array || sequence->asArray().empty() || !velocities ||
        velocities->kind() != Json::Kind::array || velocities->asArray().empty()) { error = "pattern requires pitch and velocity sequences"; return false; }
    Clip clip; clip.id = id->asString(); clip.startTick = from; clip.length = to - from;
    std::size_t step = 0;
    if (offset >= clip.length || (clip.length - offset) / grid > 100000) { error = "pattern offset or step count is out of range"; return false; }
    for (std::uint64_t tick = offset; tick < clip.length; tick += grid, ++step) {
        std::vector<int> pitches;
        if (!pitchesAt(sequence->asArray()[step % sequence->asArray().size()], pitches, error)) return false;
        std::int64_t velocity = 0;
        if (!integer(&velocities->asArray()[step % velocities->asArray().size()], 1, 127, velocity)) { error = "invalid pattern velocity"; return false; }
        if (pitches.empty()) continue;
        auto length = duration;
        if (tick + length > clip.length) {
            if (!(command.find("truncate") && command.find("truncate")->asBool())) { error = "pattern note crosses clip end; choose explicit truncate"; return false; }
            length = static_cast<std::uint32_t>(clip.length - tick);
        }
        for (const auto pitch : pitches) {
            if (pitch < 0 || pitch > 127 || clip.notes.size() >= 100000) { error = "pattern pitch or note count is out of range"; return false; }
            Note note; note.id = clip.id + ":note:" + std::to_string(clip.notes.size() + 1);
            note.tick = static_cast<std::uint32_t>(tick); note.duration = length;
            note.pitch = static_cast<std::uint8_t>(pitch); note.velocity = static_cast<std::uint8_t>(velocity);
            clip.notes.push_back(std::move(note));
        }
    }
    idMap.set(clip.id, Json::string(clip.id)); track->clips.push_back(std::move(clip)); return true;
}
} // namespace nodsynth::song
