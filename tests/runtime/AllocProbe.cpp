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
    const auto compiled = nodsynth::compiler::GraphCompiler{}.compile(nodsynth::nodes::sinePatch(), registry);
    if (!compiled.graph) return 2;
    auto prepared = nodsynth::runtime::preparePlan(
        *compiled.graph, registry, nodsynth::nodes::builtinImplementations(), engine.config(), engine.voices(), 0.01);
    if (!prepared.plan || !engine.stage(std::move(prepared.plan)).accepted) return 2;
    engine.process(nullptr, 0, 1, {});

    std::vector<float> left(128, 0.f);
    std::vector<float> right(128, 0.f);
    float* outputs[] = {left.data(), right.data()};
    const nodsynth::runtime::MidiEvent note{0, nodsynth::runtime::MidiType::noteOn, 0, 69, 100};
    hits.store(0);
    armed.store(true);
    engine.process(outputs, 2, 128, std::span<const nodsynth::runtime::MidiEvent>(&note, 1));
    engine.process(outputs, 2, 128, {});
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
