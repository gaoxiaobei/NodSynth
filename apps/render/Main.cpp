#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/runtime/Engine.h>
#include <nodsynth/runtime/WavFile.h>

namespace {
void usage() {
    std::cerr << "usage: nod_render --output FILE [--sample-rate 48000] [--block-size 128] [--seconds 2]\n"
              << "       nod_render --soak [--sample-rate 48000] [--block-size 128] [--seconds 600]\n";
}

int soak(double sampleRate, std::uint32_t blockSize, double seconds) {
    nodsynth::runtime::EngineConfig config;
    config.audio.sampleRate = sampleRate;
    config.audio.maxFrames = blockSize;
    config.audio.voiceCount = 16;
    nodsynth::runtime::Engine engine(config);
    const auto registry = nodsynth::nodes::builtinRegistry();
    const auto compilePlan = [&](const nodsynth::model::GraphSnapshot& graph) {
        const auto compiled = nodsynth::compiler::GraphCompiler{}.compile(graph, registry);
        if (!compiled.graph) return nodsynth::runtime::PlanResult{};
        return nodsynth::runtime::preparePlan(
            *compiled.graph, registry, nodsynth::nodes::builtinImplementations(), engine.config(), engine.voices(), 0.01);
    };
    auto initial = compilePlan(nodsynth::nodes::sinePatch());
    if (!initial.plan || !engine.stage(std::move(initial.plan)).accepted) return 2;
    auto next = compilePlan(nodsynth::nodes::filterPatch());
    if (!next.plan) return 2;

    const auto total = static_cast<std::uint64_t>(std::llround(seconds * sampleRate));
    const auto switchAt = std::min<std::uint64_t>(total / 2, static_cast<std::uint64_t>(sampleRate * 30.0));
    std::vector<float> left(blockSize, 0.f);
    std::vector<float> right(blockSize, 0.f);
    std::vector<float> loads;
    loads.reserve(static_cast<std::size_t>(total / blockSize + 8));
    bool finite = true;
    bool switched = false;
    float switchMax = 0.f;
    int switchBlocks = 0;
    std::vector<nodsynth::runtime::MidiEvent> notes;
    for (std::uint8_t note = 48; note < 64; ++note) notes.push_back({0, nodsynth::runtime::MidiType::noteOn, 0, note, 96});
    for (std::uint64_t rendered = 0; rendered < total;) {
        const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(blockSize, total - rendered));
        float* outputs[] = {left.data(), right.data()};
        if (rendered == 0) engine.process(outputs, 2, chunk, notes);
        else engine.process(outputs, 2, chunk, {});
        engine.reclaim();
        for (std::uint32_t frame = 0; frame < chunk; ++frame) {
            if (!std::isfinite(left[frame]) || !std::isfinite(right[frame])) finite = false;
        }
        const float load = engine.telemetry().lastLoad;
        loads.push_back(load);
        if (!switched && rendered + chunk >= switchAt) {
            switched = engine.stage(std::move(next.plan)).accepted;
        }
        if (switched && switchBlocks < 40) {
            switchMax = std::max(switchMax, load);
            ++switchBlocks;
        }
        rendered += chunk;
    }
    std::vector<nodsynth::runtime::MidiEvent> offs;
    for (std::uint8_t note = 48; note < 64; ++note) offs.push_back({0, nodsynth::runtime::MidiType::noteOff, 0, note, 0});
    const auto tail = static_cast<std::uint32_t>(sampleRate * 2.0);
    for (std::uint32_t rendered = 0; rendered < tail;) {
        const auto chunk = std::min(blockSize, tail - rendered);
        float* outputs[] = {left.data(), right.data()};
        if (rendered == 0) engine.process(outputs, 2, chunk, offs);
        else engine.process(outputs, 2, chunk, {});
        engine.reclaim();
        rendered += chunk;
    }
    const auto voices = engine.telemetry().activeVoices;
    const auto xruns = engine.telemetry().xruns;
    std::sort(loads.begin(), loads.end());
    const auto percentile = [&](double fraction) {
        if (loads.empty()) return 0.f;
        const auto index = std::min(loads.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(loads.size() - 1)));
        return loads[index];
    };
    const double blockMs = 1000.0 * static_cast<double>(blockSize) / sampleRate;
    std::cout << "callbacks=" << loads.size() << " xruns=" << xruns << " voicesAfterRelease=" << voices
              << " finite=" << (finite ? 1 : 0) << " switched=" << (switched ? 1 : 0) << " p50ms=" << percentile(0.50) * blockMs
              << " p99ms=" << percentile(0.99) * blockMs << " maxms=" << (loads.empty() ? 0.f : loads.back()) * blockMs
              << " switchMaxms=" << switchMax * blockMs << " blockms=" << blockMs << '\n';
    if (!finite || !switched || xruns != 0 || voices != 0) return 4;
    return 0;
}

bool readNumber(int& index, int argc, char** argv, const char* name, double& value) {
    if (index + 1 >= argc) {
        std::cerr << "missing value for " << name << '\n';
        return false;
    }
    try {
        value = std::stod(argv[++index]);
    } catch (const std::exception&) {
        std::cerr << "invalid number for " << name << '\n';
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    std::string output;
    double sampleRate = 48000.0;
    double blockSize = 128.0;
    double seconds = 2.0;
    bool soakMode = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--soak") {
            soakMode = true;
            if (seconds == 2.0) seconds = 600.0;
        } else if (argument == "--output") {
            if (index + 1 >= argc) return usage(), 1;
            output = argv[++index];
        } else if (argument == "--sample-rate") {
            if (!readNumber(index, argc, argv, "--sample-rate", sampleRate)) return 1;
        } else if (argument == "--block-size") {
            if (!readNumber(index, argc, argv, "--block-size", blockSize)) return 1;
        } else if (argument == "--seconds") {
            if (!readNumber(index, argc, argv, "--seconds", seconds)) return 1;
        } else {
            usage();
            return 1;
        }
    }
    const double secondLimit = soakMode ? 600.0 : 30.0;
    if ((!soakMode && output.empty()) || !nodsynth::runtime::validSampleRate(sampleRate) || seconds <= 0.0 || seconds > secondLimit ||
        blockSize < 1.0 || blockSize > nodsynth::runtime::kMaxFrames) {
        usage();
        return 1;
    }
    if (soakMode) return soak(sampleRate, static_cast<std::uint32_t>(blockSize), seconds);

    nodsynth::runtime::EngineConfig config;
    config.audio.sampleRate = sampleRate;
    config.audio.maxFrames = static_cast<std::uint32_t>(blockSize);
    config.audio.voiceCount = 16;
    nodsynth::runtime::Engine engine(config);
    const auto registry = nodsynth::nodes::builtinRegistry();
    const auto compiled = nodsynth::compiler::GraphCompiler{}.compile(nodsynth::nodes::sinePatch(), registry);
    if (!compiled.graph) return 2;
    auto prepared = nodsynth::runtime::preparePlan(
        *compiled.graph, registry, nodsynth::nodes::builtinImplementations(), engine.config(), engine.voices(), 0.01);
    if (!prepared.plan || !engine.stage(std::move(prepared.plan)).accepted) return 2;

    const auto total = static_cast<std::uint32_t>(std::llround(seconds * sampleRate));
    const auto noteOff = static_cast<std::uint32_t>(std::llround(sampleRate));
    std::vector<float> left(total, 0.f);
    std::vector<float> right(total, 0.f);
    bool started = false;
    bool released = false;
    for (std::uint32_t rendered = 0; rendered < total;) {
        const auto chunk = std::min(config.audio.maxFrames, total - rendered);
        std::vector<nodsynth::runtime::MidiEvent> events;
        if (!started) {
            events.push_back({0, nodsynth::runtime::MidiType::noteOn, 0, 69, 100});
            started = true;
        }
        if (!released && rendered <= noteOff && noteOff < rendered + chunk) {
            events.push_back({noteOff - rendered, nodsynth::runtime::MidiType::noteOff, 0, 69, 0});
            released = true;
        }
        float* outputs[2] = {left.data() + rendered, right.data() + rendered};
        engine.process(outputs, 2, chunk, events);
        rendered += chunk;
    }

    nodsynth::runtime::WavData wav;
    wav.sampleRate = static_cast<std::uint32_t>(sampleRate);
    wav.channels = 2;
    wav.interleaved.resize(static_cast<std::size_t>(total) * 2);
    for (std::uint32_t frame = 0; frame < total; ++frame) {
        wav.interleaved[static_cast<std::size_t>(frame) * 2] = left[frame];
        wav.interleaved[static_cast<std::size_t>(frame) * 2 + 1] = right[frame];
    }
    std::string error;
    if (!nodsynth::runtime::writeWav(output, wav, error)) {
        std::cerr << error << '\n';
        return 3;
    }
    std::cout << "wrote " << total << " frames\n";
    return 0;
}
