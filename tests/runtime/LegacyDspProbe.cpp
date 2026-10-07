#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/runtime/Engine.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace nodsynth::nodes {
const runtime::ImplementationRegistry& legacyBuiltinImplementations();
}

int main() {
    using namespace nodsynth;
    const auto registry = nodes::builtinRegistry();
    std::uint64_t checked = 0;
    for (const auto rate : {44100u, 48000u, 96000u}) for (const auto block : {64u, 128u, 512u})
        for (const auto& graph : {nodes::sinePatch(), nodes::filterPatch(), nodes::delayPatch()}) {
            const auto compiled = compiler::GraphCompiler{}.compile(graph, registry);
            if (!compiled.graph) return 2;
            runtime::EngineConfig config;
            config.audio.sampleRate = rate; config.audio.maxFrames = block; config.parameterSmoothSeconds = 0;
            runtime::Engine current(config), legacy(config);
            const auto prepare = [&](runtime::Engine& engine, const runtime::ImplementationRegistry& implementations) {
                auto plan = runtime::preparePlan(*compiled.graph, registry, implementations, engine.config(), engine.voices(), 0);
                return plan.plan && engine.stage(std::move(plan.plan)).accepted;
            };
            if (!prepare(current, nodes::builtinImplementations()) || !prepare(legacy, nodes::legacyBuiltinImplementations())) return 3;
            std::vector<float> aLeft(block), aRight(block), bLeft(block), bRight(block);
            std::array<runtime::MidiEvent, 16> events{};
            for (std::uint8_t voice = 0; voice < 16; ++voice)
                events[voice] = {0, runtime::MidiType::noteOn, 0, static_cast<std::uint8_t>(48 + voice), 100};
            for (std::uint32_t start = 0; start < rate; start += block) {
                const auto frames = std::min(block, rate - start);
                const auto midi = start == 0 ? std::span<const runtime::MidiEvent>(events) : std::span<const runtime::MidiEvent>{};
                float* a[]{aLeft.data(), aRight.data()}; float* b[]{bLeft.data(), bRight.data()};
                current.process(a, 2, frames, midi); legacy.process(b, 2, frames, midi);
                for (std::uint32_t frame = 0; frame < frames; ++frame) {
                    if (aLeft[frame] != bLeft[frame] || aRight[frame] != bRight[frame] || !std::isfinite(aLeft[frame])) {
                        std::cerr << "legacy mismatch rate=" << rate << " block=" << block << " sample=" << start + frame << '\n';
                        return 1;
                    }
                    checked += 2;
                }
            }
        }
    std::cout << "Legacy DSP is sample-identical: " << checked << " channel samples, 27 configurations\n";
    return 0;
}
