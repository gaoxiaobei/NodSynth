#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <nodsynth/daw/Timeline.h>
#include <nodsynth/persist/Json.h>
#include <nodsynth/song/SongDocument.h>

using namespace nodsynth;

TEST_CASE("the timeline places notes and edits them through the song command", "[daw]") {
    song::SongDocument song;
    song.ppq = 480;
    song.tempo = {{0, 500000}};
    song::Track track;
    track.id = "melody";
    song::Clip clip;
    clip.id = "clip";
    clip.notes.push_back({"n1", 0, 480, 60, 100, 0});
    track.clips.push_back(std::move(clip));
    song.tracks.push_back(std::move(track));

    daw::ViewMetrics metrics;
    const auto boxes = daw::layoutNotes(song, metrics);
    REQUIRE(boxes.size() == 1);
    REQUIRE(boxes[0].x == metrics.headerWidth);
    REQUIRE(boxes[0].width == metrics.pixelsPerQuarter);
    REQUIRE(daw::noteAt(boxes, boxes[0].x + 1, boxes[0].y + 1) == &boxes[0]);
    REQUIRE(daw::noteAt(boxes, 0, 0) == nullptr);
    REQUIRE(daw::tickAtPixel(metrics.headerWidth + metrics.pixelsPerQuarter, song.ppq, metrics) == 480);
    REQUIRE(daw::playheadPixel(song, 24000, 48000, metrics) == metrics.headerWidth + metrics.pixelsPerQuarter);

    persist::Json batch = persist::Json::object();
    batch.set("schemaVersion", persist::Json::number(1));
    persist::Json commands = persist::Json::array();
    commands.push(daw::moveNoteCommand("melody", "n1", 960, 64));
    batch.set("commands", std::move(commands));
    const auto applied = song::applyCommands(song, batch, song.revision);
    REQUIRE(applied.ok);
    REQUIRE(song.tracks[0].clips[0].notes[0].tick == 960);
    REQUIRE(song.tracks[0].clips[0].notes[0].pitch == 64);
    REQUIRE(song::undoSong(song).ok);
    REQUIRE(song.tracks[0].clips[0].notes[0].tick == 0);
    REQUIRE(song.tracks[0].clips[0].notes[0].pitch == 60);

    daw::PianoRollMetrics piano;
    const auto roll = daw::layoutPianoRoll(song, 0, piano);
    REQUIRE(roll.size() == 1);
    REQUIRE(roll[0].y == (static_cast<int>(piano.highPitch) - 60) * piano.rowHeight);
    REQUIRE(daw::pitchAtPianoRow(roll[0].y + 1, piano) == 60);
    const auto strips = daw::layoutMixer(song);
    REQUIRE(strips.size() == 1);
    REQUIRE(daw::gainAtFader(strips[0].y + 28, strips[0]) == 2.0);
    REQUIRE(std::fabs(daw::panAtStrip(strips[0].x + strips[0].width / 2, strips[0])) < 0.05);
    persist::Json mix = persist::Json::object();
    mix.set("schemaVersion", persist::Json::number(1));
    persist::Json mixCommands = persist::Json::array();
    mixCommands.push(daw::setGainCommand("melody", 0.5));
    mixCommands.push(daw::setPanCommand("melody", -0.25));
    mix.set("commands", std::move(mixCommands));
    REQUIRE(song::applyCommands(song, mix, song.revision).ok);
    REQUIRE(song.tracks[0].gain == 0.5);
    REQUIRE(song.tracks[0].pan == -0.25);
}
