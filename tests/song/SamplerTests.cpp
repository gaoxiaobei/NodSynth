#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodsynth/song/Sampler.h>
#include <nodsynth/song/SongRenderer.h>
#include <nodsynth/runtime/WavFile.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>

using namespace nodsynth;
namespace {
struct Fixture {
    std::filesystem::path root;
    song::SongDocument document;
    Fixture(const char* name) : root(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(root); std::filesystem::create_directories(root);
        document = song::createSong({120, 4, 4, 480, 1}); document.baseDirectory = root;
        song::Track track; track.id = "drums"; track.gainMode = "multiply"; track.panMode = "balance";
        document.tracks.push_back(track);
    }
    ~Fixture() { std::filesystem::remove_all(root); }
    void sample(const char* id, float level, std::uint32_t channels = 1) {
        runtime::WavData wav; wav.sampleRate = 48000; wav.channels = channels;
        wav.interleaved.resize(4800 * channels);
        for (std::size_t frame = 0; frame < 4800; ++frame) for (std::uint32_t c = 0; c < channels; ++c)
            wav.interleaved[frame * channels + c] = level * (c ? -.5f : 1.f);
        std::string error; REQUIRE(runtime::writeWav(root / (std::string(id) + ".wav"), wav, error));
        song::Resource r; r.id = id; r.path = std::string(id) + ".wav"; r.kind = "sample";
        r.hash = song::hashFile(root / r.path); r.license = "CC0-1.0"; r.source = "test signal";
        document.resources.push_back(r);
    }
    song::Instrument instrument(std::vector<song::SampleLayer> samples) {
        song::Instrument result; result.id = "sampler"; result.kind = song::InstrumentKind::sampler;
        result.samples = std::move(samples); return result;
    }
};
song::SampleLayer layer(const char* id, int note = 60) {
    song::SampleLayer result; result.resourceId = id; result.rootNote = result.lowNote = result.highNote = note;
    result.fadeInMs = result.fadeOutMs = 0; return result;
}
std::vector<float> playback(song::OneShotSampler& sampler, std::uint32_t count, std::uint32_t block,
    const std::vector<runtime::MidiEvent>& events) {
    std::vector<float> output(count * 2), left(block), right(block);
    for (std::uint32_t begin = 0; begin < count; begin += block) {
        std::vector<runtime::MidiEvent> current;
        const auto frames = std::min(block, count - begin);
        for (auto event : events) if (event.sampleOffset >= begin && event.sampleOffset < begin + frames) {
            event.sampleOffset -= begin; current.push_back(event);
        }
        sampler.process(left.data(), right.data(), frames, current);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            output[(begin + frame) * 2] = left[frame]; output[(begin + frame) * 2 + 1] = right[frame];
        }
    }
    return output;
}
}

TEST_CASE("Q2 sampler selects velocity layers rotates deterministically and ignores note off", "[production][sampler]") {
    Fixture f("nod-sampler-layers"); f.sample("quiet", .2f); f.sample("loud-a", .4f); f.sample("loud-b", .6f);
    auto quiet = layer("quiet"), loudA = layer("loud-a"), loudB = layer("loud-b");
    quiet.highVelocity = 63; loudA.lowVelocity = loudB.lowVelocity = 64;
    const auto instrument = f.instrument({quiet, loudA, loudB});
    song::OneShotSampler sampler; std::string error;
    REQUIRE(sampler.prepare(instrument, f.document, f.root, 48000, error));
    const std::vector<runtime::MidiEvent> events{{0,runtime::MidiType::noteOn,0,60,127},
        {1,runtime::MidiType::noteOff,0,60,0}, {5000,runtime::MidiType::noteOn,0,60,127},
        {10000,runtime::MidiType::noteOn,0,60,63}, {15000,runtime::MidiType::noteOn,0,60,127}};
    const auto reference = playback(sampler, 19000, 128, events);
    REQUIRE(reference[200] == Catch::Approx(.4).margin(1e-6));
    REQUIRE(reference[10200] == Catch::Approx(.6).margin(1e-6));
    REQUIRE(reference[20200] == Catch::Approx(.2 * 63 / 127).margin(1e-6));
    REQUIRE(reference[30200] == Catch::Approx(.4).margin(1e-6));
    for (auto block : {64u,512u}) { sampler.reset(); REQUIRE(playback(sampler, 19000, block, events) == reference); }
    sampler.reset();
    const auto channel = playback(sampler, 800, 128, {{0,runtime::MidiType::noteOn,1,60,127}});
    REQUIRE(channel[200] == Catch::Approx(.4).margin(1e-6));
}

TEST_CASE("Q2 sampler choke scope stereo endpoints and stolen voices are bounded", "[production][sampler]") {
    Fixture f("nod-sampler-choke"); f.sample("open", .4f, 2); f.sample("closed", .2f);
    auto open = layer("open",46), closed = layer("closed",42); open.chokeGroup = closed.chokeGroup = 1;
    song::OneShotSampler sampler; std::string error;
    REQUIRE(sampler.prepare(f.instrument({open,closed}), f.document, f.root, 48000, error));
    const auto out = playback(sampler, 1000, 64, {{0,runtime::MidiType::noteOn,0,46,127},{300,runtime::MidiType::noteOn,1,42,127}});
    REQUIRE(out[400] == Catch::Approx(.4).margin(1e-6));
    REQUIRE(out[401] == Catch::Approx(-.2).margin(1e-6));
    REQUIRE(out[1000] == Catch::Approx(.2).margin(1e-6));
    REQUIRE(out[1001] == Catch::Approx(.2).margin(1e-6));
    sampler.reset();
    std::vector<runtime::MidiEvent> dense;
    for (std::uint32_t frame = 0; frame < 64; ++frame) dense.push_back({frame,runtime::MidiType::noteOn,0,46,127});
    dense.push_back({100,runtime::MidiType::allSoundOff,0,0,0});
    const auto stolen = playback(sampler, 400, 128, dense);
    REQUIRE(sampler.stolenNotes() == 48);
    for (float sample : stolen) REQUIRE(std::isfinite(sample));
    REQUIRE(stolen[700] == 0); REQUIRE(stolen[701] == 0);
}

TEST_CASE("Q2 sampler tuning duration and fades survive supported sample rates", "[production][sampler]") {
    Fixture f("nod-sampler-resample");
    runtime::WavData wav; wav.sampleRate = 48000; wav.channels = 1; wav.interleaved.resize(4800);
    for (std::size_t i = 0; i < wav.interleaved.size(); ++i) wav.interleaved[i] = static_cast<float>(.5 * std::sin(2 * std::numbers::pi * 1000 * i / 48000));
    std::string error; REQUIRE(runtime::writeWav(f.root / "tone.wav", wav, error));
    song::Resource r; r.id = "tone"; r.path = "tone.wav"; r.kind = "sample"; r.hash = song::hashFile(f.root / r.path); f.document.resources.push_back(r);
    auto config = layer("tone"); config.highNote = 72; config.fadeInMs = 1; config.fadeOutMs = 5;
    for (auto rate : {44100u,48000u,96000u}) {
        song::OneShotSampler sampler; REQUIRE(sampler.prepare(f.instrument({config}), f.document, f.root, rate, error));
        const auto output = playback(sampler, rate / 10, 128, {{0,runtime::MidiType::noteOn,0,72,127}});
        REQUIRE(output[0] == 0);
        int crossings = 0;
        for (std::uint32_t i = rate / 100; i < rate / 25; ++i) if (output[i*2] > 0 && output[(i-1)*2] <= 0) ++crossings;
        REQUIRE(crossings >= 59); REQUIRE(crossings <= 61);
        for (std::uint32_t i = rate * 6 / 100; i < rate / 10; ++i) REQUIRE(output[i*2] == 0);
        sampler.reset(); REQUIRE(playback(sampler, rate / 10, 512, {{0,runtime::MidiType::noteOn,0,72,127}}) == output);
    }
    for (std::size_t i=0;i<wav.interleaved.size();++i) wav.interleaved[i]=static_cast<float>(.5*std::sin(2*std::numbers::pi*18000*i/48000));
    REQUIRE(runtime::writeWav(f.root/"tone.wav",wav,error));f.document.resources.back().hash=song::hashFile(f.root/"tone.wav");
    config.lowNote=config.highNote=72;
    song::OneShotSampler high;REQUIRE(high.prepare(f.instrument({config}),f.document,f.root,48000,error));
    const auto rejected=playback(high,2400,128,{{0,runtime::MidiType::noteOn,0,72,127}});
    double energy=0;for(std::size_t frame=480;frame<1920;++frame) energy+=rejected[frame*2]*rejected[frame*2];
    REQUIRE(std::sqrt(energy/1440)<.0001);
}

TEST_CASE("Q2 sampler rejects ambiguous ranges changed files nonfinite data and budget overruns", "[production][sampler]") {
    Fixture f("nod-sampler-invalid"); f.sample("a",.2f); f.sample("b",.4f);
    auto a = layer("a"), b = layer("b"); b.lowVelocity = 64;
    song::OneShotSampler sampler; std::string error;
    REQUIRE_FALSE(sampler.prepare(f.instrument({a,b}), f.document, f.root, 48000,error)); REQUIRE(error.find("overlapping") != std::string::npos);
    error.clear(); REQUIRE_FALSE(sampler.prepare(f.instrument({a}), f.document, f.root, 48000,error, 100)); REQUIRE(error.find("budget") != std::string::npos);
    a.lowNote = 0; error.clear(); REQUIRE_FALSE(sampler.prepare(f.instrument({a}), f.document, f.root,48000,error)); REQUIRE(error.find("transpose") != std::string::npos);
    a = layer("a"); a.startSeconds = .2; error.clear(); REQUIRE_FALSE(sampler.prepare(f.instrument({a}),f.document,f.root,48000,error)); REQUIRE(error.find("range") != std::string::npos);
    a = layer("a"); f.document.resources[0].hash = "changed"; error.clear(); REQUIRE_FALSE(sampler.prepare(f.instrument({a}),f.document,f.root,48000,error)); REQUIRE(error.find("hash") != std::string::npos);
    runtime::WavData invalid; invalid.sampleRate=48000; invalid.channels=1; invalid.interleaved={std::numeric_limits<float>::quiet_NaN()};
    REQUIRE(runtime::writeWav(f.root / "a.wav",invalid,error)); f.document.resources[0].hash=song::hashFile(f.root / "a.wav");
    error.clear(); REQUIRE_FALSE(sampler.prepare(f.instrument({a}),f.document,f.root,48000,error)); REQUIRE(error.find("nonfinite") != std::string::npos);
}

TEST_CASE("Q2 punch kick edition preserves other drums and deterministic licensed assets", "[production][sampler][punch-kick]") {
    Fixture f("nod-punch-kit");std::string error;
    REQUIRE(song::createSampleKit(f.root/"original",error));
    REQUIRE(song::createSampleKit(f.root/"punch",error,true));
    REQUIRE(song::createSampleKit(f.root/"repeat",error,true));
    for(const auto* name:{"kick","snare","clap","closed-hat","open-hat","percussion"}) for(int alternate=1;alternate<=2;++alternate) {
        const auto file=std::string(name)+"-"+std::to_string(alternate)+".wav";
        REQUIRE(song::hashFile(f.root/"punch"/file)==song::hashFile(f.root/"repeat"/file));
        if(std::string_view(name)!="kick") REQUIRE(song::hashFile(f.root/"original"/file)==song::hashFile(f.root/"punch"/file));
        else {
            REQUIRE(song::hashFile(f.root/"original"/file)!=song::hashFile(f.root/"punch"/file));
            runtime::WavData original,punch;
            REQUIRE(runtime::readWav(f.root/"original"/file,original,error));REQUIRE(runtime::readWav(f.root/"punch"/file,punch,error));
            REQUIRE(punch.interleaved.front()==0);REQUIRE(punch.interleaved.back()==0);
            double originalBody=0,punchBody=0;
            for(std::size_t frame=0;frame<punch.interleaved.size();++frame) {
                REQUIRE(std::isfinite(punch.interleaved[frame]));REQUIRE(std::fabs(punch.interleaved[frame])<1);
                if(frame>=2400 && frame<7200) {originalBody+=original.interleaved[frame]*original.interleaved[frame];punchBody+=punch.interleaved[frame]*punch.interleaved[frame];}
            }
            REQUIRE(punchBody>originalBody);
        }
    }
    std::ifstream manifestFile(f.root/"punch"/"kit.json");
    const std::string text((std::istreambuf_iterator<char>(manifestFile)),std::istreambuf_iterator<char>());
    const auto manifest=persist::Json::parse(text,error);REQUIRE(manifest);
    REQUIRE(manifest->find("license")->asString()=="CC0-1.0");REQUIRE(manifest->find("edition")->asNumber()==2);
    REQUIRE(manifest->find("auditionStatus")->asString()=="unheard");
}

TEST_CASE("Q2 drum kit binding collection undo relocation and cached remix preserve samples", "[production][sampler][song]") {
    Fixture f("nod-sampler-portable"); std::string error;
    REQUIRE(song::createSampleKit(f.root / "kit", error));
    REQUIRE_FALSE(song::createSampleKit(f.root / "kit", error));
    auto batch = persist::Json::parse(R"({"schemaVersion":1,"commands":[{"op":"bind-sample-kit","track":"drums","kit":"kit/kit.json"}]})", error);
    REQUIRE(batch); REQUIRE(song::applyCommands(f.document,*batch).ok);
    REQUIRE(f.document.resources.size() == 12);
    for (const auto& resource : f.document.resources) { REQUIRE(resource.license == "CC0-1.0"); REQUIRE(resource.path.rfind("assets/audio/",0) == 0); }
    REQUIRE(song::undoSong(f.document).ok); REQUIRE(f.document.tracks[0].instrumentId.empty()); REQUIRE(song::redoSong(f.document).ok);
    song::Clip clip; clip.id="beat"; clip.length=1920;
    clip.notes={{"kick",0,120,36,110,0},{"open",120,120,46,100,0},{"closed",240,120,42,100,0},{"snare",480,120,38,120,0}};
    f.document.tracks[0].clips.push_back(clip);
    REQUIRE(song::saveSong(f.root / "song.json",f.document,error));
    const auto copy = f.root / "relocated";
    std::filesystem::create_directories(copy); std::filesystem::copy(f.root / "assets",copy / "assets",std::filesystem::copy_options::recursive);
    std::filesystem::copy_file(f.root / "song.json",copy / "song.json");
    auto moved = song::loadSong(copy / "song.json",error); REQUIRE(moved); REQUIRE(song::validateRenderReady(*moved).ok);
    song::SongRenderOptions options; options.tailSeconds=.1; options.cacheDirectory=f.root / "cache";
    REQUIRE(song::renderSong(*moved,options,f.root / "cold.wav",f.root / "stems").ok);
    const auto warm = song::renderSong(*moved,options,f.root / "warm.wav",{}); REQUIRE(warm.ok); REQUIRE(warm.cacheHit);
    REQUIRE(song::hashFile(f.root / "cold.wav") == song::hashFile(f.root / "warm.wav"));
    moved->tracks[0].gain=.7;
    REQUIRE(song::renderSong(*moved,options,f.root / "remix.wav",{}).ok);
    options.useCache=false;
    REQUIRE(song::renderSong(*moved,options,f.root / "fresh.wav",{}).ok);
    REQUIRE(song::hashFile(f.root / "remix.wav") == song::hashFile(f.root / "fresh.wav"));
    moved->tracks[0].clips[0].notes[0].pitch=1;
    REQUIRE_FALSE(song::validateDocument(*moved).ok);
}

TEST_CASE("WAV reader handles padded metadata PCM24 and rejects truncated RIFF data", "[production][sampler][wav]") {
    Fixture f("nod-wave-chunks");
    {
        std::ofstream output(f.root / "pcm24.wav",std::ios::binary);
        output.write("RIFF",4); runtime::writeLe32(output,52); output.write("WAVEJUNK",8); runtime::writeLe32(output,1); output.write("x\0",2);
        output.write("fmt ",4); runtime::writeLe32(output,16); runtime::writeLe16(output,1); runtime::writeLe16(output,1);
        runtime::writeLe32(output,48000); runtime::writeLe32(output,144000); runtime::writeLe16(output,3); runtime::writeLe16(output,24);
        output.write("data",4); runtime::writeLe32(output,6); const unsigned char data[]{0,0,64,0,0,128}; output.write(reinterpret_cast<const char*>(data),6);
    }
    runtime::WavData wave; std::string error;
    REQUIRE(runtime::readWav(f.root / "pcm24.wav",wave,error)); REQUIRE(wave.interleaved == std::vector<float>{.5f,-1.f});
    std::filesystem::resize_file(f.root / "pcm24.wav",58);
    REQUIRE_FALSE(runtime::readWav(f.root / "pcm24.wav",wave,error));
}
