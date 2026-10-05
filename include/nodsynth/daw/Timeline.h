#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nodsynth/persist/Json.h>
#include <nodsynth/song/SongDocument.h>

namespace nodsynth::daw {
struct ViewMetrics {
    int pixelsPerQuarter{48};
    int trackHeight{72};
    int headerWidth{128};
    int minNoteWidth{4};
};

struct NoteBox {
    std::string trackId;
    std::string clipId;
    std::string noteId;
    std::uint32_t clipStart{0};
    std::uint32_t tick{0};
    std::uint32_t duration{0};
    std::uint8_t pitch{60};
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

[[nodiscard]] std::vector<NoteBox> layoutNotes(const song::SongDocument& song, ViewMetrics metrics = {});
[[nodiscard]] const NoteBox* noteAt(const std::vector<NoteBox>& notes, int x, int y);
[[nodiscard]] std::uint32_t tickAtPixel(int x, std::uint16_t ppq, ViewMetrics metrics = {});
[[nodiscard]] std::uint8_t pitchAtPixel(int y, int trackIndex, ViewMetrics metrics = {});
[[nodiscard]] persist::Json moveNoteCommand(const std::string& trackId, const std::string& noteId, std::uint32_t tick, std::uint8_t pitch);
[[nodiscard]] int playheadPixel(const song::SongDocument& song, std::int64_t sample, std::uint32_t sampleRate, ViewMetrics metrics = {});

struct PianoRollMetrics {
    int keyWidth{56};
    int rowHeight{10};
    int pixelsPerQuarter{48};
    int minNoteWidth{4};
    std::uint8_t lowPitch{36};
    std::uint8_t highPitch{96};
};

struct MixerMetrics {
    int stripWidth{72};
    int stripGap{8};
    int faderTop{28};
    int faderHeight{120};
};

struct MixerStrip {
    std::string trackId;
    int x{0};
    int y{0};
    int width{0};
    int height{0};
    int faderY{0};
    int panX{0};
};

[[nodiscard]] std::vector<NoteBox> layoutPianoRoll(const song::SongDocument& song, std::size_t trackIndex, PianoRollMetrics metrics = {});
[[nodiscard]] std::uint8_t pitchAtPianoRow(int y, PianoRollMetrics metrics = {});
[[nodiscard]] std::vector<MixerStrip> layoutMixer(const song::SongDocument& song, MixerMetrics metrics = {});
[[nodiscard]] double gainAtFader(int y, const MixerStrip& strip, MixerMetrics metrics = {});
[[nodiscard]] double panAtStrip(int x, const MixerStrip& strip);
[[nodiscard]] persist::Json setGainCommand(const std::string& trackId, double gain);
[[nodiscard]] persist::Json setPanCommand(const std::string& trackId, double pan);
} // namespace nodsynth::daw
