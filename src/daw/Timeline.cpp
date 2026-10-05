#include <nodsynth/daw/Timeline.h>

#include <algorithm>

namespace nodsynth::daw {
namespace {
int quarterPixels(std::uint32_t ticks, std::uint16_t ppq, const ViewMetrics& metrics)
{
    const auto quarters = ppq == 0 ? 480 : ppq;
    return static_cast<int>((static_cast<std::uint64_t>(ticks) * static_cast<std::uint64_t>(metrics.pixelsPerQuarter)) / quarters);
}
} // namespace

std::vector<NoteBox> layoutNotes(const song::SongDocument& song, ViewMetrics metrics)
{
    if (metrics.trackHeight < 16) metrics.trackHeight = 16;
    if (metrics.pixelsPerQuarter < 4) metrics.pixelsPerQuarter = 4;
    std::vector<NoteBox> boxes;
    const auto noteHeight = std::max(4, metrics.trackHeight / 16);
    for (std::size_t trackIndex = 0; trackIndex < song.tracks.size(); ++trackIndex) {
        const auto& track = song.tracks[trackIndex];
        const int laneTop = static_cast<int>(trackIndex) * metrics.trackHeight;
        for (const auto& clip : track.clips) {
            for (const auto& note : clip.notes) {
                const auto absolute = static_cast<std::uint64_t>(clip.startTick) + note.tick;
                NoteBox box;
                box.trackId = track.id;
                box.clipId = clip.id;
                box.noteId = note.id;
                box.clipStart = clip.startTick;
                box.tick = note.tick;
                box.duration = note.duration;
                box.pitch = note.pitch;
                box.x = metrics.headerWidth + quarterPixels(static_cast<std::uint32_t>(std::min<std::uint64_t>(absolute, 0xffffffffu)), song.ppq, metrics);
                box.width = std::max(metrics.minNoteWidth, quarterPixels(std::max<std::uint32_t>(note.duration, 1), song.ppq, metrics));
                const int span = std::max(1, metrics.trackHeight - noteHeight);
                box.y = laneTop + ((127 - static_cast<int>(note.pitch)) * span) / 127;
                box.height = noteHeight;
                boxes.push_back(std::move(box));
            }
        }
    }
    return boxes;
}

const NoteBox* noteAt(const std::vector<NoteBox>& notes, int x, int y)
{
    const NoteBox* found = nullptr;
    for (const auto& note : notes) {
        if (x >= note.x && x < note.x + note.width && y >= note.y && y < note.y + note.height) found = &note;
    }
    return found;
}

std::uint32_t tickAtPixel(int x, std::uint16_t ppq, ViewMetrics metrics)
{
    const auto quarters = ppq == 0 ? 480 : ppq;
    const auto local = std::max(0, x - metrics.headerWidth);
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(local) * quarters) / std::max(1, metrics.pixelsPerQuarter));
}

std::uint8_t pitchAtPixel(int y, int trackIndex, ViewMetrics metrics)
{
    if (metrics.trackHeight < 16) metrics.trackHeight = 16;
    const int noteHeight = std::max(4, metrics.trackHeight / 16);
    const int span = std::max(1, metrics.trackHeight - noteHeight);
    const int local = std::clamp(y - trackIndex * metrics.trackHeight, 0, span);
    const int pitch = 127 - (local * 127) / span;
    return static_cast<std::uint8_t>(std::clamp(pitch, 0, 127));
}

persist::Json moveNoteCommand(const std::string& trackId, const std::string& noteId, std::uint32_t tick, std::uint8_t pitch)
{
    persist::Json command = persist::Json::object();
    command.set("op", persist::Json::string("move-note"));
    command.set("track", persist::Json::string(trackId));
    command.set("note", persist::Json::string(noteId));
    command.set("tick", persist::Json::number(tick));
    command.set("pitch", persist::Json::number(pitch));
    return command;
}

std::vector<NoteBox> layoutPianoRoll(const song::SongDocument& song, std::size_t trackIndex, PianoRollMetrics metrics)
{
    std::vector<NoteBox> boxes;
    if (trackIndex >= song.tracks.size()) return boxes;
    if (metrics.rowHeight < 4) metrics.rowHeight = 4;
    if (metrics.highPitch < metrics.lowPitch) std::swap(metrics.highPitch, metrics.lowPitch);
    const auto& track = song.tracks[trackIndex];
    for (const auto& clip : track.clips) {
        for (const auto& note : clip.notes) {
            if (note.pitch < metrics.lowPitch || note.pitch > metrics.highPitch) continue;
            const auto absolute = static_cast<std::uint64_t>(clip.startTick) + note.tick;
            NoteBox box;
            box.trackId = track.id;
            box.clipId = clip.id;
            box.noteId = note.id;
            box.clipStart = clip.startTick;
            box.tick = note.tick;
            box.duration = note.duration;
            box.pitch = note.pitch;
            ViewMetrics time;
            time.headerWidth = metrics.keyWidth;
            time.pixelsPerQuarter = metrics.pixelsPerQuarter;
            time.minNoteWidth = metrics.minNoteWidth;
            box.x = time.headerWidth + quarterPixels(static_cast<std::uint32_t>(std::min<std::uint64_t>(absolute, 0xffffffffu)), song.ppq, time);
            box.width = std::max(time.minNoteWidth, quarterPixels(std::max<std::uint32_t>(note.duration, 1), song.ppq, time));
            box.y = (static_cast<int>(metrics.highPitch) - static_cast<int>(note.pitch)) * metrics.rowHeight;
            box.height = std::max(4, metrics.rowHeight - 1);
            boxes.push_back(std::move(box));
        }
    }
    return boxes;
}

std::uint8_t pitchAtPianoRow(int y, PianoRollMetrics metrics)
{
    if (metrics.rowHeight < 4) metrics.rowHeight = 4;
    if (metrics.highPitch < metrics.lowPitch) std::swap(metrics.highPitch, metrics.lowPitch);
    const int row = std::max(0, y) / metrics.rowHeight;
    const int pitch = static_cast<int>(metrics.highPitch) - row;
    return static_cast<std::uint8_t>(std::clamp(pitch, static_cast<int>(metrics.lowPitch), static_cast<int>(metrics.highPitch)));
}

std::vector<MixerStrip> layoutMixer(const song::SongDocument& song, MixerMetrics metrics)
{
    if (metrics.stripWidth < 24) metrics.stripWidth = 24;
    if (metrics.faderHeight < 20) metrics.faderHeight = 20;
    std::vector<MixerStrip> strips;
    for (std::size_t index = 0; index < song.tracks.size(); ++index) {
        MixerStrip strip;
        strip.trackId = song.tracks[index].id;
        strip.x = static_cast<int>(index) * (metrics.stripWidth + metrics.stripGap);
        strip.y = 0;
        strip.width = metrics.stripWidth;
        strip.height = metrics.faderTop + metrics.faderHeight + 28;
        strips.push_back(std::move(strip));
    }
    return strips;
}

double gainAtFader(int y, const MixerStrip& strip, MixerMetrics metrics)
{
    if (metrics.faderHeight < 1) return 1.0;
    const auto local = std::clamp(y - (strip.y + metrics.faderTop), 0, metrics.faderHeight);
    const auto remaining = static_cast<double>(metrics.faderHeight - local) / static_cast<double>(metrics.faderHeight);
    return remaining * 2.0;
}

double panAtStrip(int x, const MixerStrip& strip)
{
    if (strip.width <= 1) return 0.0;
    const auto local = std::clamp(x - strip.x, 0, strip.width);
    return std::clamp((static_cast<double>(local) / static_cast<double>(strip.width)) * 2.0 - 1.0, -1.0, 1.0);
}

persist::Json setGainCommand(const std::string& trackId, double gain)
{
    persist::Json command = persist::Json::object();
    command.set("op", persist::Json::string("set-gain"));
    command.set("track", persist::Json::string(trackId));
    command.set("gain", persist::Json::number(gain));
    return command;
}

persist::Json setPanCommand(const std::string& trackId, double pan)
{
    persist::Json command = persist::Json::object();
    command.set("op", persist::Json::string("set-pan"));
    command.set("track", persist::Json::string(trackId));
    command.set("pan", persist::Json::number(pan));
    return command;
}

int playheadPixel(const song::SongDocument& song, std::int64_t sample, std::uint32_t sampleRate, ViewMetrics metrics)
{
    if (sample <= 0 || sampleRate == 0) return metrics.headerWidth;
    std::uint32_t low = 0;
    std::uint32_t high = std::max<std::uint32_t>(song::endTick(song), 1);
    while (low < high) {
        const auto mid = low + (high - low + 1) / 2;
        const auto at = song::sampleAtTick(song, mid, sampleRate);
        if (at && *at <= sample) low = mid;
        else high = mid - 1;
    }
    return metrics.headerWidth + quarterPixels(low, song.ppq, metrics);
}
} // namespace nodsynth::daw
