#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <nodsynth/song/Automation.h>
#include <nodsynth/song/AudioExport.h>
#include <nodsynth/song/Presets.h>
#include <nodsynth/song/SongRenderer.h>

#include <cmath>
#include <fstream>

using namespace nodsynth;
namespace {
persist::Json parse(const std::string& text) {
    std::string error; auto value = persist::Json::parse(text, error); REQUIRE(value); return *value;
}
song::ApplyResult apply(song::SongDocument& document, const std::string& commands, bool dryRun = false) {
    return song::applyCommands(document, parse("{\"schemaVersion\":1,\"commands\":" + commands + "}"), document.revision, dryRun);
}
std::filesystem::path testRoot(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path); std::filesystem::create_directories(path); return path;
}
song::SongDocument automatedSong(const std::filesystem::path& root) {
    auto document = song::createSong({120, 4, 4, 480, 1}); document.baseDirectory = root;
    const auto result = apply(document, R"([
        {"op":"create-track","id":"bass"},
        {"op":"bind-preset","track":"bass","preset":"trance-bass"},
        {"op":"add-pattern","track":"bass","id":"phrase","startBar":1,"endBar":2,
         "grid":"1/4","duration":"1/4","pitches":[48],"velocities":[100]}
    ])");
    REQUIRE(result.ok); return document;
}
}

TEST_CASE("D0 assets are collected transactionally and portable", "[iteration][song]") {
    const auto root = testRoot("nod-iteration-assets");
    auto document = song::createSong({}); document.baseDirectory = root / "original";
    std::filesystem::create_directories(document.baseDirectory);
    const auto commands = R"([{"op":"create-track","id":"kick"},{"op":"bind-preset","track":"kick","preset":"kick"}])";
    REQUIRE(apply(document, commands, true).ok);
    REQUIRE_FALSE(std::filesystem::exists(document.baseDirectory / "assets"));
    REQUIRE(document.tracks.empty());
    REQUIRE_FALSE(apply(document, R"([{"op":"create-track","id":"kick"},{"op":"bind-preset","track":"kick","preset":"kick"},{"op":"unknown"}])").ok);
    REQUIRE_FALSE(std::filesystem::exists(document.baseDirectory / "assets"));
    REQUIRE(apply(document, commands).ok);
    REQUIRE(document.resources[0].path.starts_with("assets/presets/"));
    REQUIRE(document.resources[0].presetId == "kick");
    REQUIRE(song::resourceDiagnostics(document, document.baseDirectory).asArray().empty());
    std::string error;
    REQUIRE(song::saveSong(document.baseDirectory / "song.json", document, error));
    std::filesystem::create_directories(root / "save-as");
    REQUIRE(song::saveSong(root / "save-as/song.json", document, error));
    auto savedAs = song::loadSong(root / "save-as/song.json", error); REQUIRE(savedAs);
    REQUIRE(song::resourceDiagnostics(*savedAs, savedAs->baseDirectory).asArray().empty());
    REQUIRE(song::undoSong(*savedAs).ok);
    REQUIRE(song::redoSong(*savedAs).ok);
    REQUIRE(song::resourceDiagnostics(*savedAs, savedAs->baseDirectory).asArray().empty());
    std::filesystem::copy(document.baseDirectory, root / "moved", std::filesystem::copy_options::recursive);
    std::filesystem::remove_all(document.baseDirectory);
    auto loaded = song::loadSong(root / "moved/song.json", error); REQUIRE(loaded);
    REQUIRE(song::resourceDiagnostics(*loaded, loaded->baseDirectory).asArray().empty());
    REQUIRE(apply(*loaded, R"([{"op":"add-pattern","track":"kick","id":"kick-notes","startBar":1,"endBar":2,"grid":"1/4","duration":"1/16","pitches":[36],"velocities":[100]}])").ok);
    song::SongRenderOptions options; options.useCache = false; options.tailSeconds = 0.05;
    REQUIRE(song::renderSong(*loaded, options, root / "moved.wav").ok);
    const auto asset = song::resolveResourcePath(loaded->baseDirectory, loaded->resources[0].path);
    { std::ofstream out(asset, std::ios::app); out << ' '; }
    const auto changed = song::resourceDiagnostics(*loaded, loaded->baseDirectory);
    REQUIRE(changed.asArray()[0].find("code")->asString() == "resource-hash-mismatch");
    std::filesystem::remove(asset);
    const auto missing = song::resourceDiagnostics(*loaded, loaded->baseDirectory);
    REQUIRE(missing.asArray()[0].find("actualHash")->isNull());
    REQUIRE(missing.asArray()[0].find("code")->asString() == "missing-resource");
    std::filesystem::remove_all(root);
}

TEST_CASE("D0 diff counts nested entities and same-length map changes", "[iteration][song]") {
    auto before = song::createSong({120, 4, 4, 480, 76}); auto after = before;
    for (int track = 0; track < 2; ++track) {
        song::Track item; item.id = "t" + std::to_string(track);
        song::Clip clip; clip.id = "c" + std::to_string(track); clip.length = 145920;
        for (int n = 0; n < 301; ++n) clip.notes.push_back({item.id + "n" + std::to_string(n), static_cast<std::uint32_t>(n * 480), 240, 60, 100, 0});
        item.clips.push_back(clip); after.tracks.push_back(item);
    }
    const auto diff = song::semanticDiff(before, after, false, false);
    REQUIRE(diff.find("entities")->find("note")->find("added")->asNumber() == 602);
    REQUIRE(diff.find("changedEntityCount")->asNumber() == 606);
    REQUIRE(diff.find("added")->asArray().size() == 4);
    REQUIRE(diff.dump().size() < 8192);
    const auto deleted = song::semanticDiff(after, before, false, false);
    REQUIRE(deleted.find("entities")->find("note")->find("removed")->asNumber() == 602);
    before.tempo = {{0, 500000}, {480, 600000}, {960, 500000}};
    after = before; after.tempo[1].microsecondsPerQuarter = 400000;
    REQUIRE(song::semanticDiff(before, after).find("changedEntityCount")->asNumber() == 1);
    before.timeSignatures = {{0,4,4},{1920,3,4},{3360,4,4}}; after = before; after.timeSignatures[1].numerator = 5;
    REQUIRE(song::semanticDiff(before, after).find("changedEntityCount")->asNumber() == 1);
}

TEST_CASE("D0 request content conflicts and revision identities are preserved", "[iteration][song]") {
    auto document = song::createSong({});
    auto batch = parse(R"({"schemaVersion":1,"requestId":"create","commands":[{"op":"create-track","id":"a"}]})");
    REQUIRE(song::applyCommands(document, batch).ok);
    REQUIRE(song::applyCommands(document, batch, 1).unchanged);
    auto conflicting = parse(R"({"schemaVersion":1,"requestId":"create","commands":[{"op":"create-track","id":"b"}]})");
    REQUIRE(song::applyCommands(document, conflicting).code == "request-conflict");
    const auto revision = document.revision;
    REQUIRE(song::undoSong(document).ok); REQUIRE(document.revision > revision);
    REQUIRE(song::redoSong(document).ok); REQUIRE(document.revision > revision + 1);
    REQUIRE(song::applyCommands(document, batch).unchanged);
}

TEST_CASE("D1 cutoff automation changes audio and agrees across blocks and cache", "[iteration][song]") {
    const auto root = testRoot("nod-iteration-automation"); auto document = automatedSong(root);
    REQUIRE(apply(document, R"([{"op":"create-track","id":"kick"},{"op":"bind-preset","track":"kick","preset":"kick"},
        {"op":"add-pattern","track":"kick","id":"beat","startBar":1,"endBar":2,"grid":"1/4","duration":"1/16","pitches":[36],"velocities":[100]}])").ok);
    song::SongRenderOptions options; options.tailSeconds = 0.05; options.blockSize = 128; options.cacheDirectory = root / "cache";
    REQUIRE(song::renderSong(document, options, root / "static.wav").ok);
    REQUIRE(apply(document, R"([{"op":"set-parameter-automation","track":"bass","parameter":"filter/cutoff","valueDomain":"physical","interpolation":"linear","points":[{"tick":1,"value":100},{"tick":1501,"value":9000}]}])").ok);
    const auto fresh = song::renderSong(document, options, root / "fresh.wav"); REQUIRE(fresh.ok);
    REQUIRE(fresh.cacheReason == "partial-dry-tracks");
    REQUIRE_FALSE(fresh.stems[0].cacheHit);
    REQUIRE(fresh.stems[1].cacheHit);
    REQUIRE(song::hashFile(root / "static.wav") != song::hashFile(root / "fresh.wav"));
    const auto cached = song::renderSong(document, options, root / "cached.wav"); REQUIRE(cached.ok); REQUIRE(cached.cacheHit);
    REQUIRE(song::hashFile(root / "fresh.wav") == song::hashFile(root / "cached.wav"));
    runtime::WavData reference; std::string error; REQUIRE(runtime::readWav(root / "fresh.wav", reference, error));
    for (const auto block : {64u, 512u}) {
        options.useCache = false; options.blockSize = block;
        REQUIRE(song::renderSong(document, options, root / "block.wav").ok);
        runtime::WavData other; REQUIRE(runtime::readWav(root / "block.wav", other, error));
        REQUIRE(other.interleaved.size() == reference.interleaved.size());
        double maximum = 0;
        for (std::size_t i = 0; i < other.interleaved.size(); ++i) maximum = std::max(maximum, std::fabs(static_cast<double>(other.interleaved[i] - reference.interleaved[i])));
        REQUIRE(maximum < 0.00001);
    }
    options.blockSize = 128; options.previewStartTick = 480; options.previewEndTick = 1440;
    REQUIRE(song::renderSong(document, options, root / "preroll.wav").ok);
    runtime::WavData preview; REQUIRE(runtime::readWav(root / "preroll.wav", preview, error));
    const auto origin = *song::sampleAtTick(document, 480, 48000);
    for (std::size_t i = 0; i < preview.interleaved.size(); ++i) REQUIRE(std::fabs(preview.interleaved[i] - reference.interleaved[static_cast<std::size_t>(origin) * 2 + i]) < 0.00001f);
    document.tracks[0].parameterAutomation[0].id = "missing/cutoff";
    REQUIRE_FALSE(song::renderSong(document, options, root / "bad.wav").ok);
    std::filesystem::remove_all(root);
}

TEST_CASE("D1 macros update multiple actual targets and step lanes hold base values", "[iteration][song]") {
    const auto root = testRoot("nod-iteration-macros");
    auto document = automatedSong(root);
    REQUIRE(apply(document, R"([{"op":"bind-preset","track":"bass","preset":"trance-lead"},
        {"op":"set-parameter","track":"bass","parameter":"macro:level","value":0.12},
        {"op":"set-parameter-automation","track":"bass","parameter":"filter/cutoff","valueDomain":"physical","interpolation":"step",
         "points":[{"tick":480,"value":500},{"tick":960,"value":5000}]}])").ok);
    std::string error;
    const auto& resource = document.resources.back();
    auto graph = render::loadPatch(song::resolveResourcePath(document.baseDirectory, resource.path), error); REQUIRE(graph);
    std::vector<song::PreparedLane> lanes;
    REQUIRE(song::prepareAutomation(document, document.tracks[0], *graph, nullptr, 48000, lanes, error));
    REQUIRE(lanes.size() == 4);
    REQUIRE(song::parameterAt(lanes[0], 23999) == 4200);
    REQUIRE(song::parameterAt(lanes[0], 24000) == 500);
    REQUIRE(song::parameterAt(lanes[0], 47999) == 500);
    REQUIRE(song::parameterAt(lanes[0], 48000) == 5000);
    REQUIRE(song::parameterAt(lanes[0], 96000) == 5000);
    for (std::size_t i = 1; i < lanes.size(); ++i) REQUIRE(lanes[i].base == Catch::Approx(.12));
    document.tracks[0].parameterAutomation[0].valueDomain = "normalized";
    REQUIRE_FALSE(song::validateRenderReady(document).ok);
    std::filesystem::remove_all(root);
}

TEST_CASE("D0 resource base ignores cwd and supports Unicode names", "[iteration][song]") {
    const auto root = testRoot("nod-iteration-cwd");
    const auto directory = root / std::filesystem::path(u8"song \u4e2d\u6587");
    std::filesystem::create_directories(directory);
    const auto source = directory / std::filesystem::path(u8"patch \u4e2d.json");
    std::filesystem::copy_file(song::defaultPresetsRoot() / "pad.json", source);
    auto document = song::createSong({120,4,4,480,1}); document.baseDirectory = directory;
    REQUIRE(apply(document, R"([{"op":"create-track","id":"pad"}])").ok);
    std::string error; REQUIRE(song::saveSong(directory / "song.json", document, error));
    struct RestoreCwd { std::filesystem::path previous{std::filesystem::current_path()}; ~RestoreCwd() { std::filesystem::current_path(previous); } } restore;
    const auto utf8 = source.filename().u8string();
    auto command = persist::Json::object();
    command.set("op", persist::Json::string("set-instrument"));
    command.set("track", persist::Json::string("pad"));
    command.set("patch", persist::Json::string({reinterpret_cast<const char*>(utf8.data()), utf8.size()}));
    auto commands = persist::Json::array(); commands.push(std::move(command));
    auto batch = persist::Json::object(); batch.set("schemaVersion", persist::Json::number(1)); batch.set("commands", std::move(commands));
    std::string hash;
    for (const auto& cwd : {root, directory, restore.previous}) {
        std::filesystem::current_path(cwd);
        auto loaded = song::loadSong(directory / "song.json", error); REQUIRE(loaded);
        const auto applied = song::applyCommands(*loaded, batch);
        INFO(applied.code << ": " << applied.message);
        REQUIRE(applied.ok);
        if (hash.empty()) hash = loaded->resources[0].hash;
        REQUIRE(loaded->resources[0].hash == hash);
        REQUIRE(song::resourceDiagnostics(*loaded, loaded->baseDirectory).asArray().empty());
    }
    std::filesystem::current_path(restore.previous);
    std::filesystem::remove_all(root);
}

TEST_CASE("D1 PCM export and audition records bind to actual audio", "[iteration][song]") {
    const auto root = testRoot("nod-iteration-audition");
    runtime::WavData audio; audio.sampleRate = 48000; audio.channels = 2; audio.interleaved = {0,.25f,-.5f,1};
    std::string error; double gain = 1;
    REQUIRE(song::writePcm16(root / "preview.wav", audio, false, gain, error));
    runtime::WavData decoded; REQUIRE(runtime::readWav(root / "preview.wav", decoded, error));
    REQUIRE(decoded.encoding == 1); REQUIRE(decoded.bitsPerSample == 16);
    REQUIRE(decoded.interleaved[2] == Catch::Approx(-.5));
    auto manifest = persist::Json::object(); manifest.set("renderId", persist::Json::string("render-one"));
    manifest.set("fileHash", persist::Json::string(song::hashFile(root / "preview.wav")));
    REQUIRE(song::recordAudition(root / "preview.wav", root / "records.json", manifest, "playbackStarted", "player", error));
    REQUIRE(song::auditionRecords(root / "preview.wav", root / "records.json", error).find("auditionStatus")->asString() == "unheard");
    REQUIRE(song::recordAudition(root / "preview.wav", root / "records.json", manifest, "heard", "human", error));
    REQUIRE(song::auditionRecords(root / "preview.wav", root / "records.json", error).find("auditionStatus")->asString() == "heard");
    audio.interleaved[0] = .125f;
    REQUIRE(song::writePcm16(root / "preview.wav", audio, false, gain, error));
    REQUIRE(song::auditionRecords(root / "preview.wav", root / "records.json", error).find("auditionStatus")->asString() == "unheard");
    audio.interleaved[0] = 2;
    REQUIRE_FALSE(song::writePcm16(root / "overflow.wav", audio, false, gain, error));
    REQUIRE_FALSE(std::filesystem::exists(root / "overflow.wav"));
    REQUIRE(song::writePcm16(root / "overflow.wav", audio, true, gain, error)); REQUIRE(gain < 1);
    std::filesystem::remove_all(root);
}

TEST_CASE("D2 patterns use fractions, chord voicing and independent copies", "[iteration][song]") {
    auto document = song::createSong({138,4,4,480,17});
    REQUIRE(apply(document, R"([{"op":"create-track","id":"pad"},
        {"op":"add-pattern","track":"pad","id":"chords","startBar":1,"endBar":18,"grid":"1/1","duration":"1/1",
         "pitches":[{"root":57,"quality":"minor","inversion":1},[53,57,60],[48,52,55],[55,59,62]],"velocities":[80]},
        {"op":"duplicate-clip","track":"pad","clip":"chords","id":"copy","startBar":18},
        {"op":"transpose-notes","track":"pad","clip":"copy","semitones":12},
        {"op":"scale-velocities","track":"pad","clip":"copy","factor":0.5}])").ok);
    REQUIRE(document.tracks[0].clips[0].notes.size() == 51);
    REQUIRE(document.tracks[0].clips[0].notes[0].pitch == 60);
    REQUIRE(document.tracks[0].clips[0].notes[0].velocity == 80);
    REQUIRE(document.tracks[0].clips[1].notes[0].pitch == 72);
    REQUIRE(document.tracks[0].clips[1].notes[0].velocity == 40);
    REQUIRE(song::validateDocument(document).ok);
    const auto image = song::toJson(document).dump();
    REQUIRE_FALSE(apply(document, R"([{"op":"transpose-notes","track":"pad","semitones":100}])").ok);
    REQUIRE(song::toJson(document).dump() == image);
    document.ppq = 100;
    REQUIRE_FALSE(apply(document, R"([{"op":"add-pattern","track":"pad","id":"bad","startBar":1,"endBar":2,"grid":"1/7","duration":"1/4","pitches":[60],"velocities":[80]}])").ok);
}

TEST_CASE("D2 phrases cross meter boundaries without tick rounding", "[iteration][song]") {
    auto document = song::createSong({120,4,4,480,4});
    document.timeSignatures = {{0,4,4},{1920,3,4},{4800,4,4}};
    REQUIRE(apply(document, R"([{"op":"create-track","id":"kick"},{"op":"add-pattern","track":"kick","id":"meter-beat","startBar":1,"endBar":5,
        "grid":"1/4","duration":"1/16","pitches":[36],"velocities":[100]}])").ok);
    REQUIRE(document.tracks[0].clips[0].length == 6720);
    REQUIRE(document.tracks[0].clips[0].notes.size() == 14);
    document.timeSignatures = {{0,4,4},{120,3,4}};
    std::uint32_t from = 0, to = 0; std::string error;
    REQUIRE_FALSE(song::barsToTicks(document, 1, 3, from, to, error));
}
