#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include <span>

#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/runtime/Engine.h>
#include <nodsynth/song/Sampler.h>
#include <nodsynth/song/Mixer.h>
#include <nodsynth/runtime/WavFile.h>

namespace {
std::atomic<bool> armed{false};
std::atomic<int> hits{0};
}

void* operator new(std::size_t size) {
    if (armed.load(std::memory_order_relaxed)) hits.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    nodsynth::runtime::EngineConfig config;
    config.audio.sampleRate = 48000;
    config.audio.maxFrames = 128;
    config.audio.voiceCount = 16;
    nodsynth::runtime::Engine engine(config);
    const auto registry = nodsynth::nodes::builtinRegistry();
    const auto compiled = nodsynth::compiler::GraphCompiler{}.compile(nodsynth::nodes::unisonPatch(), registry);
    if (!compiled.graph) return 2;
    auto prepared = nodsynth::runtime::preparePlan(
        *compiled.graph, registry, nodsynth::nodes::builtinImplementations(), engine.config(), engine.voices(), 0.01);
    if (!prepared.plan || !engine.stage(std::move(prepared.plan)).accepted) return 2;
    engine.process(nullptr, 0, 1, {});

    std::vector<float> left(128, 0.f);
    std::vector<float> right(128, 0.f);
    float* outputs[] = {left.data(), right.data()};
    nodsynth::runtime::MidiEvent notes[16];
    for (std::uint8_t i = 0; i < 16; ++i) notes[i] = {0, nodsynth::runtime::MidiType::noteOn, 0, static_cast<std::uint8_t>(48 + i), 100};
    const auto samplePath = std::filesystem::temp_directory_path() / "nod-alloc-sampler.wav";
    nodsynth::runtime::WavData audio; audio.sampleRate = 48000; audio.channels = 1; audio.interleaved.resize(480, .1f);
    std::string error;
    if (!nodsynth::runtime::writeWav(samplePath, audio, error)) return 2;
    nodsynth::song::SongDocument song;
    nodsynth::song::Resource resource; resource.id = "sample"; resource.path = samplePath.generic_string(); resource.kind = "sample";
    resource.hash = nodsynth::song::hashFile(samplePath); song.resources.push_back(resource);
    nodsynth::song::Instrument instrument; instrument.kind = nodsynth::song::InstrumentKind::sampler;
    nodsynth::song::SampleLayer layer; layer.resourceId = "sample"; layer.lowNote = 48; layer.highNote = 63; instrument.samples.push_back(layer);
    nodsynth::song::OneShotSampler sampler;
    if (!sampler.prepare(instrument, song, {}, 48000, error)) return 2;
    std::filesystem::remove(samplePath);
    nodsynth::song::Track track;track.id="track";track.sends={{"room",.2,false}};song.tracks.push_back(track);
    nodsynth::song::Bus bus;bus.id="room";bus.isReturn=true;
    nodsynth::effects::Config reverb;reverb.id="reverb";reverb.type="reverb";bus.inserts.push_back(reverb);song.buses.push_back(bus);
    nodsynth::song::SongMixer mixer;if(!mixer.prepare(song,48000,128,error)) return 2;
    nodsynth::effects::Config delayConfig;delayConfig.type="delay";
    auto delay=nodsynth::effects::makeEffect("delay");if(!delay->prepare(48000,128,delayConfig,error)) return 2;
    nodsynth::effects::Config eqConfig;eqConfig.type="eq";
    auto eq=nodsynth::effects::makeEffect("eq");if(!eq->prepare(48000,128,eqConfig,error)) return 2;
    nodsynth::effects::Config compressorConfig;compressorConfig.type="compressor";compressorConfig.sidechain="track";
    auto compressor=nodsynth::effects::makeEffect("compressor");if(!compressor->prepare(48000,128,compressorConfig,error)) return 2;
    nodsynth::effects::Config limiterConfig;limiterConfig.type="limiter";limiterConfig.quality="high";
    auto limiter=nodsynth::effects::makeEffect("limiter");if(!limiter->prepare(48000,128,limiterConfig,error)) return 2;
    nodsynth::effects::Config saturationConfig;saturationConfig.type="saturation";saturationConfig.quality="high";
    auto saturation=nodsynth::effects::makeEffect("saturation");if(!saturation->prepare(48000,128,saturationConfig,error)) return 2;
    std::vector<float> master(256);
    hits.store(0);
    armed.store(true);
    engine.process(outputs, 2, 128, notes);
    engine.process(outputs, 2, 128, {});
    sampler.process(left.data(), right.data(), 128, notes);
    sampler.process(left.data(), right.data(), 128, notes);
    sampler.process(left.data(), right.data(), 128, {});
    delay->process({left.data(),right.data(),128});
    const nodsynth::effects::ParameterEvent eqEvent{16,1,2000};
    eq->process({left.data(),right.data(),128,nullptr,nullptr,nullptr,{&eqEvent,1}});
    compressor->process({left.data(),right.data(),128,left.data(),right.data()});
    limiter->process({left.data(),right.data(),128});
    const nodsynth::effects::ParameterEvent driveEvent{32,0,18};
    saturation->process({left.data(),right.data(),128,nullptr,nullptr,nullptr,{&driveEvent,1}});
    mixer.setTrackInput(0,left.data(),right.data(),128);mixer.process(0,128,master.data());
    armed.store(false);
    const int allocated = hits.load();
    if (allocated != 0) {
        std::printf("audio callback allocated %d times\n", allocated);
        return 1;
    }
    for (float sample : left) {
        if (!std::isfinite(sample)) return 3;
    }
    std::printf("audio callback allocated 0 times\n");
    return 0;
}
