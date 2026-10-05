#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nodsynth/midi/Smf.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/persist/Json.h>
#include <nodsynth/persist/ProjectFile.h>
#include <nodsynth/runtime/WavFile.h>
#include <nodsynth/runtime/WavFile.h>
#include <nodsynth/song/AudioAnalysis.h>
#include <nodsynth/song/ModelClient.h>
#include <nodsynth/song/Process.h>
#include <nodsynth/song/SongDocument.h>
#include <nodsynth/song/SongRenderer.h>

using namespace nodsynth;

namespace {
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

class SmfBuilder {
public:
    std::uint16_t format{1};
    std::uint16_t ppq{480};
    std::vector<std::vector<std::uint8_t>> tracks{1};
    std::vector<std::uint32_t> endDeltas{0};

    void event(std::size_t track, std::uint32_t delta, std::initializer_list<std::uint8_t> bytes) {
        if (tracks.size() <= track) tracks.resize(track + 1);
        writeVarlen(tracks[track], delta);
        tracks[track].insert(tracks[track].end(), bytes);
    }

    void setEnd(std::size_t track, std::uint32_t delta) {
        if (endDeltas.size() <= track) endDeltas.resize(track + 1);
        endDeltas[track] = delta;
    }

    [[nodiscard]] std::vector<std::uint8_t> build() const {
        std::vector<std::uint8_t> out{'M', 'T', 'h', 'd'};
        write32(out, 6);
        write16(out, format);
        write16(out, static_cast<std::uint16_t>(tracks.size()));
        write16(out, ppq);
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            out.insert(out.end(), {'M', 'T', 'r', 'k'});
            auto body = tracks[index];
            writeVarlen(body, index < endDeltas.size() ? endDeltas[index] : 0);
            body.insert(body.end(), {0xff, 0x2f, 0x00});
            write32(out, static_cast<std::uint32_t>(body.size()));
            out.insert(out.end(), body.begin(), body.end());
        }
        return out;
    }
};

std::filesystem::path tempPath(const char* name) { return std::filesystem::temp_directory_path() / name; }

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output);
}

void savePatch(const std::filesystem::path& path, model::GraphSnapshot graph) {
    auto document = persist::projectFromGraph(std::move(graph));
    std::string error;
    REQUIRE(persist::saveProject(path, document, error));
}

double rms(const runtime::WavData& wav, std::uint32_t start, std::uint32_t end) {
    double sum = 0.0;
    std::uint32_t count = 0;
    for (std::uint32_t frame = start; frame < end && frame * wav.channels < wav.interleaved.size(); ++frame) {
        const float sample = wav.interleaved[static_cast<std::size_t>(frame) * wav.channels];
        sum += static_cast<double>(sample) * sample;
        ++count;
    }
    return count == 0 ? 0.0 : std::sqrt(sum / count);
}

int noteCount(const song::SongDocument& song) {
    int count = 0;
    for (const auto& track : song.tracks) {
        for (const auto& clip : track.clips) count += static_cast<int>(clip.notes.size());
    }
    return count;
}

SmfBuilder threeTrackFile() {
    SmfBuilder builder;
    builder.tracks.resize(4);
    builder.event(0, 0, {0xff, 0x51, 0x03, 0x07, 0xa1, 0x20});
    builder.event(0, 480, {0xff, 0x51, 0x03, 0x03, 0xd0, 0x90});
    builder.event(1, 0, {0x90, 60, 100});
    builder.setEnd(1, 480);
    builder.event(2, 0, {0x91, 64, 90});
    builder.setEnd(2, 480);
    builder.event(3, 0, {0x92, 67, 80});
    builder.setEnd(3, 480);
    return builder;
}
} // namespace

TEST_CASE("MIDI import keeps streams separate and does not assign a patch", "[song]") {
    const auto bytes = threeTrackFile().build();
    const auto parsed = midi::parse(bytes.data(), bytes.size());
    REQUIRE(parsed.status == midi::ParseStatus::ok);
    const auto imported = song::importMidi(parsed.file);
    REQUIRE(imported.ok);
    REQUIRE(imported.song.tracks.size() == 3);
    REQUIRE(imported.song.ppq == 480);
    REQUIRE(imported.song.tempo.size() == 2);
    REQUIRE(imported.song.tempo[1].microsecondsPerQuarter == 250000);
    for (const auto& track : imported.song.tracks) {
        REQUIRE(track.instrumentId.empty());
        REQUIRE(track.clips.size() == 1);
        REQUIRE(track.clips.front().notes.size() == 1);
    }
    REQUIRE(imported.song.tracks[0].sourceTrack == 1);
    REQUIRE(imported.song.tracks[0].sourceChannel == 0);
    REQUIRE(imported.song.tracks[1].sourceChannel == 1);
    REQUIRE(imported.song.tracks[2].sourceChannel == 2);
    REQUIRE(imported.song.tracks[0].clips.front().notes.front().pitch == 60);
    REQUIRE(imported.song.tracks[0].clips.front().notes.front().duration == 480);
}

TEST_CASE("song save, MIDI export, and command revision stay stable", "[song]") {
    const auto bytes = threeTrackFile().build();
    const auto parsed = midi::parse(bytes.data(), bytes.size());
    auto imported = song::importMidi(parsed.file);
    REQUIRE(imported.ok);
    const auto songPath = tempPath("nodsynth-song.nodsong.json");
    std::string error;
    REQUIRE(song::saveSong(songPath, imported.song, error));
    auto loaded = song::loadSong(songPath, error);
    REQUIRE(loaded);
    REQUIRE(loaded->tracks.size() == 3);
    REQUIRE(loaded->tracks[2].clips.front().notes.front().pitch == 67);
    REQUIRE(song::validate(*loaded).ok);

    const auto midiPath = tempPath("nodsynth-song-roundtrip.mid");
    REQUIRE(song::exportMidi(*loaded, midiPath, error));
    const auto roundtrip = midi::parseFile(midiPath);
    REQUIRE(roundtrip.status == midi::ParseStatus::ok);
    REQUIRE(roundtrip.file.ppq == 480);
    REQUIRE(roundtrip.file.tempo.size() == 2);
    REQUIRE(roundtrip.file.tempo[1].tick == 480);
    const auto again = song::importMidi(roundtrip.file);
    REQUIRE(again.ok);
    REQUIRE(noteCount(again.song) == 3);
    std::vector<std::uint8_t> pitches;
    for (const auto& track : again.song.tracks) pitches.push_back(track.clips.front().notes.front().pitch);
    REQUIRE(pitches == std::vector<std::uint8_t>{60, 64, 67});

    persist::Json batch = persist::Json::object();
    batch.set("schemaVersion", persist::Json::number(1));
    batch.set("requestId", persist::Json::string("req-add"));
    persist::Json commands = persist::Json::array();
    persist::Json add = persist::Json::object();
    add.set("op", persist::Json::string("add-note"));
    add.set("track", persist::Json::string(loaded->tracks[0].id));
    add.set("clip", persist::Json::string(loaded->tracks[0].clips[0].id));
    add.set("tick", persist::Json::number(960));
    add.set("duration", persist::Json::number(120));
    add.set("pitch", persist::Json::number(72));
    add.set("velocity", persist::Json::number(70));
    add.set("channel", persist::Json::number(0));
    commands.push(std::move(add));
    batch.set("commands", std::move(commands));
    const auto conflict = song::applyCommands(*loaded, batch, 99);
    REQUIRE_FALSE(conflict.ok);
    REQUIRE(conflict.code == "revision-conflict");
    REQUIRE(noteCount(*loaded) == 3);
    const auto applied = song::applyCommands(*loaded, batch, 1);
    REQUIRE(applied.ok);
    REQUIRE(applied.revision == 2);
    REQUIRE(noteCount(*loaded) == 4);
    const auto replay = song::applyCommands(*loaded, batch, 2);
    REQUIRE(replay.ok);
    REQUIRE(replay.unchanged);
    REQUIRE(replay.revision == 2);
    REQUIRE(noteCount(*loaded) == 4);
    std::filesystem::remove(songPath);
    std::filesystem::remove(midiPath);
}

TEST_CASE("three NodSynth tracks share a timeline and stems sum to the mix", "[song]") {
    const auto root = tempPath("nodsynth-song-render");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto sine = root / "sine.json";
    const auto filter = root / "filter.json";
    const auto delay = root / "delay.json";
    savePatch(sine, nodes::sinePatch());
    savePatch(filter, nodes::filterPatch());
    savePatch(delay, nodes::delayPatch());
    const auto bytes = threeTrackFile().build();
    const auto parsed = midi::parse(bytes.data(), bytes.size());
    auto imported = song::importMidi(parsed.file);
    REQUIRE(imported.ok);
    std::string error;
    REQUIRE(song::bindPatch(imported.song, imported.song.tracks[0].id, sine.string(), sine, error));
    REQUIRE(song::bindPatch(imported.song, imported.song.tracks[1].id, filter.string(), filter, error));
    REQUIRE(song::bindPatch(imported.song, imported.song.tracks[2].id, delay.string(), delay, error));
    imported.song.tracks[2].clips[0].startTick = 960;
    const auto onset = song::sampleAtTick(imported.song, 960, 48000);
    REQUIRE(onset.has_value());
    REQUIRE(*onset == 36000);
    const auto validation = song::validate(imported.song);
    REQUIRE(validation.ok);

    song::SongRenderOptions options;
    options.tailSeconds = 0.35;
    options.blockSize = 128;
    options.baseDirectory = root;
    const auto mixPath = root / "mix.wav";
    const auto stems = root / "stems";
    const auto report = song::renderSong(imported.song, options, mixPath, stems);
    REQUIRE(report.ok);
    REQUIRE(report.frames > 36000);
    REQUIRE(report.stems.size() == 3);

    runtime::WavData mix;
    runtime::WavData stemA;
    runtime::WavData stemB;
    runtime::WavData stemC;
    REQUIRE(runtime::readWav(mixPath, mix, error));
    REQUIRE(runtime::readWav(stems / (imported.song.tracks[0].id + ".wav"), stemA, error));
    REQUIRE(runtime::readWav(stems / (imported.song.tracks[1].id + ".wav"), stemB, error));
    REQUIRE(runtime::readWav(stems / (imported.song.tracks[2].id + ".wav"), stemC, error));
    REQUIRE(mix.interleaved.size() == stemA.interleaved.size());
    REQUIRE(stemA.interleaved.size() == stemB.interleaved.size());
    REQUIRE(stemB.interleaved.size() == stemC.interleaved.size());
    for (std::size_t index = 0; index < mix.interleaved.size(); ++index) {
        const float sum = stemA.interleaved[index] + stemB.interleaved[index] + stemC.interleaved[index];
        REQUIRE(std::fabs(sum - mix.interleaved[index]) < 1.0e-5f);
    }
    REQUIRE(rms(stemA, 2000, 16000) > 0.01);
    REQUIRE(rms(stemC, 0, 30000) < 0.0001);
    REQUIRE(rms(stemC, 37000, 45000) > 0.01);
    REQUIRE(mix.sampleRate == stemA.sampleRate);

    auto missing = imported.song;
    missing.resources[0].path = (root / "absent.json").string();
    missing.resources[0].hash.clear();
    const auto rejected = song::renderSong(missing, options, root / "missing.wav", {});
    REQUIRE_FALSE(rejected.ok);
    REQUIRE(rejected.code == "missing-patch");
    REQUIRE_FALSE(std::filesystem::exists(root / "missing.wav"));

    imported.song.tracks[0].gainAutomation.push_back({0, 0.0});
    const auto silenced = song::renderSong(imported.song, options, root / "gain.wav", root / "gain-stems");
    REQUIRE(silenced.ok);
    runtime::WavData quiet;
    REQUIRE(runtime::readWav(root / "gain-stems" / (imported.song.tracks[0].id + ".wav"), quiet, error));
    REQUIRE(rms(quiet, 2000, 16000) < 0.0001);
    std::filesystem::remove_all(root);
}

TEST_CASE("nod renders a mapped multitrack song from one command", "[song]") {
    const auto root = tempPath("nodsynth-nod-cli");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto midiPath = root / "song.mid";
    const auto songPath = root / "song.nodsong.json";
    const auto patch = root / "lead.json";
    const auto mix = root / "mix.wav";
    const auto stems = root / "stems";
    const auto report = root / "render.json";
    savePatch(patch, nodes::sinePatch());
    SmfBuilder builder;
    builder.tracks.resize(2);
    builder.event(1, 0, {0x90, 60, 100});
    builder.setEnd(1, 240);
    writeBytes(midiPath, builder.build());

    const std::filesystem::path exe = NOD_EXECUTABLE;
    const auto run = [&](const std::string& args) {
        const char* sink =
#if defined(_WIN32)
            " > nul 2>&1";
#else
            " > /dev/null 2>&1";
#endif
        std::string command = "\"" + exe.generic_string() + "\" " + args + sink;
#if defined(_WIN32)
        command = "\"" + command + "\"";
#endif
        return std::system(command.c_str());
    };
    const auto quote = [](const std::filesystem::path& path) { return "\"" + path.generic_string() + "\""; };
    const auto imported = run(
        "song import-midi " + quote(midiPath) + " --output " + quote(songPath) + " --map 1:0=" + quote(patch) + " --json");
    REQUIRE(imported == 0);
    const auto rendered = run(
        "render " + quote(songPath) + " --output " + quote(mix) + " --stems " + quote(stems) + " --report " + quote(report) +
        " --tail-seconds 0.2 --json");
    REQUIRE(rendered == 0);
    REQUIRE(std::filesystem::exists(mix));
    REQUIRE(std::filesystem::exists(stems / "track-1.wav"));
    std::string text;
    {
        std::ifstream reportFile(report);
        text.assign(std::istreambuf_iterator<char>(reportFile), std::istreambuf_iterator<char>());
    }
    std::string error;
    auto json = persist::Json::parse(text, error);
    REQUIRE(json);
    REQUIRE(json->find("status")->asString() == "ok");
    std::filesystem::remove_all(root);
}

#if defined(_WIN32)
TEST_CASE("an external renderer that exceeds its timeout fails the task", "[song]") {
    song::ProcessRequest request;
    request.arguments = {"ping", "-n", "6", "127.0.0.1"};
    request.timeoutMs = 200;
    const auto result = song::runProcess(request);
    REQUIRE(result.started);
    REQUIRE(result.timedOut);
}
#endif

TEST_CASE("NodSynth and FluidSynth share one timeline", "[song]") {
    const std::filesystem::path fluidsynth = NOD_FLUIDSYNTH;
    const std::filesystem::path soundfont = NOD_SOUNDFONT;
    if (!std::filesystem::exists(fluidsynth) || !std::filesystem::exists(soundfont)) SKIP("FluidSynth is not available");

    const auto root = tempPath("nodsynth-fluidsynth");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto patch = root / "sine.json";
    savePatch(patch, nodes::sinePatch());
    const auto bytes = threeTrackFile().build();
    const auto parsed = midi::parse(bytes.data(), bytes.size());
    auto imported = song::importMidi(parsed.file);
    REQUIRE(imported.ok);
    imported.song.tracks.resize(2);
    std::string error;
    REQUIRE(song::bindPatch(imported.song, imported.song.tracks[0].id, patch.string(), patch, error));
    REQUIRE(song::bindExternal(imported.song, imported.song.tracks[1].id, "fluidsynth", soundfont.string(), soundfont, error));
    imported.song.tracks[1].clips[0].notes.push_back({"note-late", 4800, 240, 72, 100, imported.song.tracks[1].clips[0].notes.front().channel});
    const auto late = song::sampleAtTick(imported.song, 4800, 48000);
    REQUIRE(late.has_value());

    song::SongRenderOptions options;
    options.tailSeconds = 0.2;
    options.blockSize = 128;
    options.baseDirectory = root;
    options.tools.push_back({"fluidsynth", fluidsynth, 120000});
    const auto mix = root / "mix.wav";
    const auto stems = root / "stems";
    const auto report = song::renderSong(imported.song, options, mix, stems);
    REQUIRE(report.ok);
    REQUIRE(report.stems.size() == 2);
    REQUIRE(report.stems[1].adapter == "fluidsynth");

    runtime::WavData mixWav;
    runtime::WavData nod;
    runtime::WavData external;
    REQUIRE(runtime::readWav(mix, mixWav, error));
    REQUIRE(runtime::readWav(stems / (imported.song.tracks[0].id + ".wav"), nod, error));
    REQUIRE(runtime::readWav(stems / (imported.song.tracks[1].id + ".wav"), external, error));
    REQUIRE(mixWav.interleaved.size() == nod.interleaved.size());
    REQUIRE(nod.interleaved.size() == external.interleaved.size());
    for (std::size_t index = 0; index < mixWav.interleaved.size(); ++index) {
        const float sum = nod.interleaved[index] + external.interleaved[index];
        REQUIRE(std::fabs(sum - mixWav.interleaved[index]) < 1.0e-4f);
    }
    const auto onset = [&](const runtime::WavData& wav, std::uint32_t start, float threshold) {
        for (std::uint32_t frame = start; frame * wav.channels + 1 < wav.interleaved.size(); ++frame) {
            const float left = wav.interleaved[static_cast<std::size_t>(frame) * wav.channels];
            const float right = wav.interleaved[static_cast<std::size_t>(frame) * wav.channels + 1];
            if (std::fabs(left) > threshold || std::fabs(right) > threshold) return frame;
        }
        return std::uint32_t{0xffffffffu};
    };
    const auto first = onset(external, 0, 0.05f);
    const auto second = onset(external, 48000, 0.05f);
    REQUIRE(first < 500);
    REQUIRE(second != 0xffffffffu);
    const auto expected = static_cast<std::int64_t>(*late);
    REQUIRE(std::llabs(static_cast<std::int64_t>(second) - static_cast<std::int64_t>(first) - expected) <= 24);
    REQUIRE(rms(nod, 2000, 12000) > 0.01);

    options.tools[0].executable = root / "missing-fluidsynth.exe";
    const auto missing = song::renderSong(imported.song, options, root / "failed.wav", {});
    REQUIRE_FALSE(missing.ok);
    REQUIRE(missing.code == "missing-adapter");
    REQUIRE_FALSE(std::filesystem::exists(root / "failed.wav"));

    options.tools[0].executable = fluidsynth;
    options.tools[0].timeoutMs = 1;
    const auto timedOut = song::renderSong(imported.song, options, root / "timeout.wav", {});
    REQUIRE_FALSE(timedOut.ok);
    REQUIRE(timedOut.code == "external-timeout");
    REQUIRE_FALSE(std::filesystem::exists(root / "timeout.wav"));
    std::filesystem::remove_all(root);
}

TEST_CASE("an eight-bar edit keeps the melody, can be undone, and previews from the start", "[song]") {
    const auto root = tempPath("nodsynth-arrange");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto patch = root / "sine.json";
    savePatch(patch, nodes::sinePatch());

    song::SongDocument song;
    song.ppq = 480;
    song.tempo = {{0, 500000}};
    song.timeSignatures = {{0, 4, 4}};
    auto makeTrack = [](const char* id, std::uint8_t pitch, bool notes) {
        song::Track track;
        track.id = id;
        track.name = id;
        song::Clip clip;
        clip.id = std::string(id) + "-clip";
        if (notes) {
            for (int bar = 0; bar < 8; ++bar) {
                song::Note note;
                note.id = std::string(id) + "-" + std::to_string(bar);
                note.tick = static_cast<std::uint32_t>(bar * 1920);
                note.duration = 240;
                note.pitch = pitch;
                note.velocity = 96;
                clip.notes.push_back(std::move(note));
            }
        }
        track.clips.push_back(std::move(clip));
        return track;
    };
    song.tracks.push_back(makeTrack("melody", 72, true));
    song.tracks.push_back(makeTrack("bass", 36, true));
    song.tracks.push_back(makeTrack("harmony", 67, false));
    song::Note held;
    held.id = "bass-hold";
    held.tick = 7000;
    held.duration = 2000;
    held.pitch = 40;
    held.velocity = 100;
    song.tracks[1].clips[0].notes.push_back(held);
    std::string error;
    for (auto& track : song.tracks) REQUIRE(song::bindPatch(song, track.id, patch.string(), patch, error));

    song::ModelAdapter adapter;
    adapter.executable = "powershell";
    adapter.arguments = {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", NOD_MODEL_ADAPTER};
    const auto proposal = song::proposeEdits(song, "keep the melody, rewrite the bass from bar 4, and add harmony", adapter);
    REQUIRE(proposal.ok);
    REQUIRE(song.tracks[0].clips[0].notes[0].pitch == 72);
    auto batch = proposal.batch;
    batch.set("requestId", persist::Json::string("arrange-1"));
    const auto applied = song::applyCommands(song, batch, 1);
    REQUIRE(applied.ok);
    REQUIRE(applied.revision == 2);
    REQUIRE(applied.diff.find("removed")->asArray().size() == 4);
    REQUIRE(applied.diff.find("added")->asArray().size() == 8);
    for (int bar = 0; bar < 8; ++bar) {
        const auto melody = "melody-" + std::to_string(bar);
        const auto found = std::find_if(song.tracks[0].clips[0].notes.begin(), song.tracks[0].clips[0].notes.end(), [&](const song::Note& note) {
            return note.id == melody;
        });
        REQUIRE(found != song.tracks[0].clips[0].notes.end());
        REQUIRE(found->pitch == 72);
    }
    const auto region = song::querySong(song, 7680, 15360);
    int regionPitches = 0;
    for (const auto& note : region.find("notes")->asArray()) {
        if (note.find("track")->asString() == "melody" && note.find("pitch")->asNumber() == 72) ++regionPitches;
    }
    REQUIRE(regionPitches == 4);
    REQUIRE(region.find("capabilities")->find("undo")->asBool());

    const auto songPath = root / "song.nodsong.json";
    REQUIRE(song::saveSong(songPath, song, error));
    auto loaded = song::loadSong(songPath, error);
    REQUIRE(loaded);
    REQUIRE(song::undoSong(*loaded).ok);
    REQUIRE(loaded->revision == 1);
    const auto restored = std::find_if(loaded->tracks[1].clips[0].notes.begin(), loaded->tracks[1].clips[0].notes.end(), [](const song::Note& note) {
        return note.id == "bass-4";
    });
    REQUIRE(restored != loaded->tracks[1].clips[0].notes.end());
    REQUIRE(restored->pitch == 36);
    REQUIRE(loaded->tracks[2].clips[0].notes.empty());
    REQUIRE(song::redoSong(*loaded).ok);
    REQUIRE(loaded->revision == 2);
    REQUIRE_FALSE(loaded->tracks[2].clips[0].notes.empty());
    const auto replay = song::applyCommands(*loaded, batch, 2);
    REQUIRE(replay.ok);
    REQUIRE(replay.unchanged);

    song::SongRenderOptions options;
    options.tailSeconds = 0.1;
    options.blockSize = 512;
    options.baseDirectory = root;
    options.previewStartTick = 7200;
    options.previewEndTick = 9000;
    const auto preview = song::renderSong(*loaded, options, root / "preview.wav", root / "preview-stems");
    REQUIRE(preview.ok);
    REQUIRE(preview.revision == 2);
    const auto origin = song::sampleAtTick(*loaded, 7200, 48000);
    REQUIRE(origin.has_value());
    REQUIRE(preview.originSample == static_cast<std::uint64_t>(*origin));
    REQUIRE(preview.frames > 90000);
    REQUIRE(preview.frames < 120000);
    runtime::WavData previewWav;
    REQUIRE(runtime::readWav(root / "preview.wav", previewWav, error));
    REQUIRE(rms(previewWav, 2000, 16000) > 0.01);
    const auto analysis = song::analyzeWav(root / "preview.wav");
    REQUIRE(analysis.ok);
    REQUIRE(analysis.peak > 0.01f);
    REQUIRE(analysis.loudnessLufs.has_value());
    REQUIRE(*analysis.loudnessLufs < 0.0);
    REQUIRE(*analysis.loudnessLufs > -70.0);

    song::SongRenderOptions full = options;
    full.previewStartTick.reset();
    full.previewEndTick.reset();
    const auto mix = song::renderSong(*loaded, full, root / "mix.wav", root / "stems");
    REQUIRE(mix.ok);
    REQUIRE(mix.revision == 2);
    REQUIRE(mix.stems.size() == 3);
    REQUIRE(song::exportMidi(*loaded, root / "song.mid", error));
    const auto midiFile = midi::parseFile(root / "song.mid");
    REQUIRE(midiFile.status == midi::ParseStatus::ok);
    int melodyNotes = 0;
    int bassNotes = 0;
    int harmonyNotes = 0;
    for (const auto& event : midiFile.file.events) {
        if (event.kind != midi::EventKind::noteOn || event.data2 == 0) continue;
        if (event.data1 == 72) ++melodyNotes;
        if (event.data1 == 43) ++bassNotes;
        if (event.data1 == 67) ++harmonyNotes;
    }
    REQUIRE(melodyNotes == 8);
    REQUIRE(bassNotes == 4);
    REQUIRE(harmonyNotes == 4);

    runtime::WavData silence;
    silence.sampleRate = 48000;
    silence.channels = 2;
    silence.interleaved.assign(48000, 0.f);
    const auto silentPath = root / "silence.wav";
    REQUIRE(runtime::writeWav(silentPath, silence, error));
    const auto silent = song::analyzeWav(silentPath);
    REQUIRE(silent.ok);
    REQUIRE(silent.peak == 0.f);
    REQUIRE(silent.silentFrames == silence.interleaved.size() / silence.channels);
    REQUIRE_FALSE(silent.loudnessLufs.has_value());
    std::filesystem::remove_all(root);
}

#ifdef NOD_VST3_WORKER
TEST_CASE("a VST3 instrument renders in a worker and a failed plugin cancels the mix", "[song]") {
    const std::filesystem::path worker = NOD_VST3_WORKER;
    const std::filesystem::path plugin = NOD_VST3_PLUGIN;
    if (!std::filesystem::exists(worker) || !std::filesystem::exists(plugin)) SKIP("VST3 worker is not available");

    song::SongDocument song;
    song.ppq = 480;
    song.tempo.push_back({0, 500000});
    song::Resource resource;
    resource.id = "plugin";
    resource.kind = "vst3";
    resource.path = plugin.string();
    song.resources.push_back(resource);
    song::Instrument instrument;
    instrument.id = "lead";
    instrument.kind = song::InstrumentKind::vst3;
    instrument.adapter = "vst3";
    instrument.resourceId = resource.id;
    instrument.name = "NodSynth";
    song.instruments.push_back(instrument);
    song::Track track;
    track.id = "lead-track";
    track.name = "Lead";
    track.instrumentId = instrument.id;
    song::Clip clip;
    clip.id = "clip";
    clip.notes.push_back({"n1", 0, 480, 60, 100, 0});
    track.clips.push_back(std::move(clip));
    song.tracks.push_back(std::move(track));

    song::SongRenderOptions options;
    options.tailSeconds = 0.3;
    options.blockSize = 128;
    options.tools.push_back({"vst3", worker, 60000});
    const auto root = tempPath("nodsynth-vst3-worker");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto mix = root / "mix.wav";
    const auto report = song::renderSong(song, options, mix, root / "stems");
    REQUIRE(report.ok);
    REQUIRE(report.stems.size() == 1);
    REQUIRE(report.stems[0].adapter == "vst3");
    runtime::WavData wav;
    std::string error;
    REQUIRE(runtime::readWav(mix, wav, error));
    REQUIRE(rms(wav, 0, 8000) > 0.01);

    song.resources[0].path = worker.string();
    const auto failed = root / "failed.wav";
    const auto rejected = song::renderSong(song, options, failed, {});
    REQUIRE_FALSE(rejected.ok);
    REQUIRE(rejected.code == "external-failed");
    REQUIRE_FALSE(std::filesystem::exists(failed));
    std::filesystem::remove_all(root);
}

TEST_CASE("VST3 parameter automation changes the rendered level", "[song]") {
    const std::filesystem::path worker = NOD_VST3_WORKER;
    const std::filesystem::path plugin = NOD_VST3_PLUGIN;
    if (!std::filesystem::exists(worker) || !std::filesystem::exists(plugin)) SKIP("VST3 worker is not available");

    song::SongDocument song;
    song.ppq = 480;
    song.tempo.push_back({0, 500000});
    song::Resource resource;
    resource.id = "plugin";
    resource.kind = "vst3";
    resource.path = plugin.string();
    song.resources.push_back(resource);
    song::Instrument instrument;
    instrument.id = "lead";
    instrument.kind = song::InstrumentKind::vst3;
    instrument.adapter = "vst3";
    instrument.resourceId = resource.id;
    instrument.name = "NodSynth";
    song.instruments.push_back(instrument);
    song::Track track;
    track.id = "lead-track";
    track.instrumentId = instrument.id;
    song::Clip clip;
    clip.id = "clip";
    clip.notes.push_back({"n1", 0, 1920, 60, 100, 0});
    track.clips.push_back(std::move(clip));
    song::ParameterLane lane;
    lane.id = "Gain";
    lane.points.push_back({0, 0.0});
    lane.points.push_back({960, 1.0});
    track.parameterAutomation.push_back(std::move(lane));
    song.tracks.push_back(std::move(track));

    song::SongRenderOptions options;
    options.tailSeconds = 0.2;
    options.blockSize = 128;
    options.tools.push_back({"vst3", worker, 60000});
    const auto root = tempPath("nodsynth-vst3-automation");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto mix = root / "mix.wav";
    const auto report = song::renderSong(song, options, mix, {});
    REQUIRE(report.ok);
    runtime::WavData wav;
    std::string error;
    REQUIRE(runtime::readWav(mix, wav, error));
    const auto early = rms(wav, 1000, 8000);
    const auto late = rms(wav, 50000, 70000);
    REQUIRE(late > 0.02);
    REQUIRE(late > early * 4.0);
    std::filesystem::remove_all(root);
}

#ifdef NOD_LATENCY_PLUGIN
TEST_CASE("reported plugin latency is removed so the note stays on the downbeat", "[song]") {
    const std::filesystem::path worker = NOD_VST3_WORKER;
    const std::filesystem::path plugin = NOD_LATENCY_PLUGIN;
    if (!std::filesystem::exists(worker) || !std::filesystem::exists(plugin)) SKIP("latency probe is not available");

    song::SongDocument song;
    song.ppq = 480;
    song.tempo.push_back({0, 500000});
    song::Resource resource;
    resource.id = "probe-plugin";
    resource.kind = "vst3";
    resource.path = plugin.string();
    song.resources.push_back(resource);
    song::Instrument instrument;
    instrument.id = "probe";
    instrument.kind = song::InstrumentKind::vst3;
    instrument.adapter = "vst3";
    instrument.resourceId = resource.id;
    instrument.name = "LatencyProbe";
    song.instruments.push_back(instrument);
    song::Track track;
    track.id = "probe-track";
    track.instrumentId = instrument.id;
    song::Clip clip;
    clip.id = "clip";
    clip.notes.push_back({"hit", 0, 480, 60, 100, 0});
    track.clips.push_back(std::move(clip));
    song.tracks.push_back(std::move(track));

    song::SongRenderOptions options;
    options.tailSeconds = 0.05;
    options.blockSize = 64;
    options.tools.push_back({"vst3", worker, 60000});
    const auto root = tempPath("nodsynth-latency");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto report = song::renderSong(song, options, root / "mix.wav", root / "stems");
    REQUIRE(report.ok);
    REQUIRE(report.stems.size() == 1);
    REQUIRE(report.stems[0].latencySamples == 128);
    runtime::WavData wav;
    std::string error;
    REQUIRE(runtime::readWav(root / "stems" / "probe-track.wav", wav, error));
    std::size_t peakAt = 0;
    float peak = 0.f;
    for (std::size_t frame = 0; frame < wav.interleaved.size() / 2; ++frame) {
        const float value = std::max(std::fabs(wav.interleaved[frame * 2]), std::fabs(wav.interleaved[frame * 2 + 1]));
        if (value > peak) {
            peak = value;
            peakAt = frame;
        }
    }
    REQUIRE(peak > 0.2f);
    REQUIRE(peakAt == 0);
    std::filesystem::remove_all(root);
}
#endif
#endif

TEST_CASE("parameter automation round-trips through the song command", "[song]") {
    song::SongDocument song;
    song.ppq = 480;
    song::Track track;
    track.id = "lead";
    song.tracks.push_back(std::move(track));
    persist::Json batch = persist::Json::object();
    batch.set("schemaVersion", persist::Json::number(1));
    persist::Json commands = persist::Json::array();
    persist::Json command = persist::Json::object();
    command.set("op", persist::Json::string("set-parameter-automation"));
    command.set("track", persist::Json::string("lead"));
    command.set("parameter", persist::Json::string("Gain"));
    persist::Json points = persist::Json::array();
    persist::Json start = persist::Json::object();
    start.set("tick", persist::Json::number(0));
    start.set("value", persist::Json::number(0));
    persist::Json end = persist::Json::object();
    end.set("tick", persist::Json::number(480));
    end.set("value", persist::Json::number(1));
    points.push(std::move(end));
    points.push(std::move(start));
    command.set("points", std::move(points));
    commands.push(std::move(command));
    batch.set("commands", std::move(commands));
    const auto applied = song::applyCommands(song, batch, song.revision);
    REQUIRE(applied.ok);
    REQUIRE(song.tracks[0].parameterAutomation.size() == 1);
    REQUIRE(song.tracks[0].parameterAutomation[0].points.size() == 2);
    REQUIRE(song.tracks[0].parameterAutomation[0].points[0].tick == 0);
    REQUIRE(song.tracks[0].parameterAutomation[0].points[1].value == 1.0);
    const auto root = tempPath("nodsynth-parameter");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto path = root / "song.json";
    std::string error;
    REQUIRE(song::saveSong(path, song, error));
    auto loaded = song::loadSong(path, error);
    REQUIRE(loaded);
    REQUIRE(loaded->tracks[0].parameterAutomation[0].id == "Gain");
    REQUIRE(loaded->tracks[0].parameterAutomation[0].points[1].tick == 480);
    REQUIRE(song::undoSong(song).ok);
    REQUIRE(song.tracks[0].parameterAutomation.empty());
    std::filesystem::remove_all(root);
}

TEST_CASE("a model adapter proposes an edit without changing the song until it is applied", "[song]") {
    const std::filesystem::path script = NOD_MODEL_ADAPTER;
    song::SongDocument song;
    song.ppq = 480;
    song.tempo.push_back({0, 500000});
    song::Track track;
    track.id = "melody";
    song::Clip clip;
    clip.id = "clip";
    clip.notes.push_back({"n1", 0, 480, 60, 100, 0});
    track.clips.push_back(std::move(clip));
    song.tracks.push_back(std::move(track));

    song::ModelAdapter adapter;
    adapter.executable = "powershell";
    adapter.arguments = {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script.string()};
    const auto proposal = song::proposeEdits(song, "raise the melody a whole step", adapter);
    REQUIRE(proposal.ok);
    REQUIRE(song.tracks[0].clips[0].notes[0].pitch == 60);
    auto batch = proposal.batch;
    batch.set("requestId", persist::Json::string("raise-melody"));
    const auto applied = song::applyCommands(song, batch, song.revision);
    REQUIRE(applied.ok);
    REQUIRE(song.tracks[0].clips[0].notes[0].pitch == 62);
    const auto repeated = song::applyCommands(song, batch, song.revision);
    REQUIRE(repeated.ok);
    REQUIRE(repeated.unchanged);
    REQUIRE(song.tracks[0].clips[0].notes[0].pitch == 62);
    REQUIRE(song::undoSong(song).ok);
    REQUIRE(song.tracks[0].clips[0].notes[0].pitch == 60);

    const auto root = tempPath("nodsynth-model");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto songPath = root / "song.json";
    const auto commandsPath = root / "commands.json";
    std::string error;
    REQUIRE(song::saveSong(songPath, song, error));
    const std::filesystem::path exe = NOD_EXECUTABLE;
    const auto quote = [](const std::filesystem::path& path) { return "\"" + path.generic_string() + "\""; };
    std::string command = "\"" + exe.generic_string() + "\" song propose " + quote(songPath) +
                          " --adapter powershell --adapter-arg -NoProfile --adapter-arg -ExecutionPolicy --adapter-arg Bypass --adapter-arg -File --adapter-arg " +
                          quote(script) + " --instruction \"raise the melody\" --output " + quote(commandsPath) + " --json > nul 2>&1";
#if defined(_WIN32)
    command = "\"" + command + "\"";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
    std::string proposedText;
    {
        std::ifstream proposed(commandsPath);
        proposedText.assign(std::istreambuf_iterator<char>(proposed), std::istreambuf_iterator<char>());
    }
    REQUIRE(proposedText.find("move-note") != std::string::npos);
    std::filesystem::remove_all(root);
}
