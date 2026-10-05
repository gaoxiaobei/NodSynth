#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/midi/Smf.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/render/OfflineRenderer.h>
#include <nodsynth/runtime/Engine.h>
#include <nodsynth/runtime/WavFile.h>

namespace {
void usage() {
    std::cerr << "usage: nod_render --output FILE [--sample-rate 48000] [--block-size 128] [--seconds 2]\n"
              << "       nod_render --soak [--sample-rate 48000] [--block-size 128] [--seconds 600]\n"
              << "       nod_render --midi FILE --output FILE [--patch FILE] [--sample-rate 48000] [--block-size 128]\n"
              << "                 [--tail-seconds 2] [--tail-threshold AMPLITUDE] [--max-tail-seconds 8]\n"
              << "                 [--track N] [--channel 0-15] [--merge-channels] [--loose] [--json] [--report FILE]\n"
              << "       nod_render --midi FILE --inspect\n"
              << "       nod_render --midi FILE --validate [--loose]\n"
              << "One patch plays the selected track and channel. --merge-channels sends every channel to that same patch.\n"
              << "It does not restore a multi-timbre General MIDI file.\n";
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

bool readIndex(int& index, int argc, char** argv, const char* name, long& value) {
    double number = 0.0;
    if (!readNumber(index, argc, argv, name, number)) return false;
    if (number < 0.0 || number != std::floor(number)) {
        std::cerr << "invalid number for " << name << '\n';
        return false;
    }
    value = static_cast<long>(number);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    std::string output;
    std::string midiPath;
    std::string patchPath;
    std::string reportPath;
    double sampleRate = 48000.0;
    double blockSize = 128.0;
    double seconds = 2.0;
    double tailSeconds = 2.0;
    bool soakMode = false;
    bool inspect = false;
    bool validate = false;
    bool mergeChannels = false;
    bool loose = false;
    bool jsonOutput = false;
    bool thresholdTail = false;
    bool haveTail = false;
    double tailThreshold = 0.0001;
    double maxTailSeconds = 8.0;
    std::optional<long> track;
    std::optional<long> channel;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--soak") {
            soakMode = true;
            if (seconds == 2.0) seconds = 600.0;
        } else if (argument == "--inspect") {
            inspect = true;
        } else if (argument == "--validate") {
            validate = true;
        } else if (argument == "--merge-channels") {
            mergeChannels = true;
        } else if (argument == "--loose") {
            loose = true;
        } else if (argument == "--json") {
            jsonOutput = true;
        } else if (argument == "--tail-threshold") {
            if (!readNumber(index, argc, argv, "--tail-threshold", tailThreshold)) return 1;
            thresholdTail = true;
        } else if (argument == "--max-tail-seconds") {
            if (!readNumber(index, argc, argv, "--max-tail-seconds", maxTailSeconds)) return 1;
        } else if (argument == "--output") {
            if (index + 1 >= argc) return usage(), 1;
            output = argv[++index];
        } else if (argument == "--midi") {
            if (index + 1 >= argc) return usage(), 1;
            midiPath = argv[++index];
        } else if (argument == "--patch") {
            if (index + 1 >= argc) return usage(), 1;
            patchPath = argv[++index];
        } else if (argument == "--report") {
            if (index + 1 >= argc) return usage(), 1;
            reportPath = argv[++index];
        } else if (argument == "--sample-rate") {
            if (!readNumber(index, argc, argv, "--sample-rate", sampleRate)) return 1;
        } else if (argument == "--block-size") {
            if (!readNumber(index, argc, argv, "--block-size", blockSize)) return 1;
        } else if (argument == "--seconds") {
            if (!readNumber(index, argc, argv, "--seconds", seconds)) return 1;
        } else if (argument == "--tail-seconds") {
            if (!readNumber(index, argc, argv, "--tail-seconds", tailSeconds)) return 1;
            haveTail = true;
        } else if (argument == "--track") {
            long value = 0;
            if (!readIndex(index, argc, argv, "--track", value)) return 1;
            track = value;
        } else if (argument == "--channel") {
            long value = 0;
            if (!readIndex(index, argc, argv, "--channel", value)) return 1;
            if (value > 15) {
                std::cerr << "channel must be from 0 to 15\n";
                return 1;
            }
            channel = value;
        } else {
            usage();
            return 1;
        }
    }
    if (soakMode) {
        if (!nodsynth::runtime::validSampleRate(sampleRate) || seconds <= 0.0 || seconds > 600.0 || blockSize < 1.0 ||
            blockSize > nodsynth::runtime::kMaxFrames) {
            usage();
            return 1;
        }
        return soak(sampleRate, static_cast<std::uint32_t>(blockSize), seconds);
    }
    if (!midiPath.empty()) {
        const auto parsed = nodsynth::midi::parseFile(midiPath);
        if (parsed.status != nodsynth::midi::ParseStatus::ok) {
            std::cerr << parsed.message << '\n';
            return 1;
        }
        if (inspect) {
            std::cout << nodsynth::render::inspectMidi(parsed.file).dump() << '\n';
            return 0;
        }
        if (validate && output.empty()) {
            const auto mode = loose ? nodsynth::midi::RenderMode::loose : nodsynth::midi::RenderMode::strict;
            const auto capability = nodsynth::midi::assess(parsed.file, mode);
            nodsynth::render::RenderReport report;
            report.ok = !capability.rejected;
            report.code = capability.rejected ? "unsupported-event" : "";
            report.message = capability.rejected ? "strict render rejected unsupported MIDI events" : "valid";
            report.diagnostics = capability.unsupported;
            std::cout << nodsynth::render::reportJson(report, &parsed.file).dump() << '\n';
            return capability.rejected ? 3 : 0;
        }
        if (output.empty() || !nodsynth::runtime::validSampleRate(sampleRate) || blockSize < 1.0 || blockSize > nodsynth::runtime::kMaxFrames ||
            tailSeconds < 0.0) {
            usage();
            return 1;
        }
        nodsynth::model::GraphSnapshot graph = nodsynth::nodes::sinePatch();
        if (!patchPath.empty()) {
            std::string error;
            auto loaded = nodsynth::render::loadPatch(patchPath, error);
            if (!loaded) {
                std::cerr << error << '\n';
                return 2;
            }
            graph = std::move(*loaded);
        }
        nodsynth::render::RenderOptions options;
        options.sampleRate = sampleRate;
        options.blockSize = static_cast<std::uint32_t>(blockSize);
        options.tailSeconds = haveTail ? tailSeconds : 2.0;
        options.tailMode = thresholdTail ? nodsynth::render::TailMode::threshold : nodsynth::render::TailMode::fixed;
        options.tailThreshold = tailThreshold;
        options.maxTailSeconds = maxTailSeconds;
        options.mergeChannels = mergeChannels;
        options.mode = loose ? nodsynth::midi::RenderMode::loose : nodsynth::midi::RenderMode::strict;
        options.midiHash = nodsynth::render::hashFile(midiPath);
        if (!patchPath.empty()) options.patchHash = nodsynth::render::hashFile(patchPath);
        if (track) options.selection.track = static_cast<std::uint16_t>(*track);
        if (channel) options.selection.channel = static_cast<std::uint8_t>(*channel);
        const auto rendered = nodsynth::render::renderMidi(graph, parsed.file, options, output);
        if (!reportPath.empty()) {
            std::ofstream report(reportPath, std::ios::binary | std::ios::trunc);
            if (!report) {
                std::cerr << "failed to write report\n";
                return 4;
            }
            report << nodsynth::render::reportJson(rendered, &parsed.file).dump() << '\n';
        }
        if (jsonOutput) std::cout << nodsynth::render::reportJson(rendered, &parsed.file).dump() << '\n';
        if (!rendered.ok) {
            std::cerr << rendered.message << '\n';
            if (rendered.code == "compile-failed" || rendered.code == "prepare-failed" || rendered.code == "stage-failed") return 2;
            if (rendered.code == "output-io" || rendered.code == "riff-limit") return 4;
            return 3;
        }
        if (!jsonOutput) std::cout << "wrote " << rendered.frames << " frames\n";
        return 0;
    }

    const auto frames = static_cast<std::uint64_t>(std::llround(seconds * sampleRate));
    const auto dataBytes = frames * 2ull * sizeof(float);
    if (output.empty() || !nodsynth::runtime::validSampleRate(sampleRate) || seconds <= 0.0 || dataBytes > 0xffffffffu - 36u ||
        blockSize < 1.0 || blockSize > nodsynth::runtime::kMaxFrames) {
        usage();
        return 1;
    }

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

    const auto noteOff = static_cast<std::uint64_t>(std::llround(sampleRate));
    const auto block = config.audio.maxFrames;
    std::vector<float> left(block, 0.f);
    std::vector<float> right(block, 0.f);
    std::vector<float> interleaved(static_cast<std::size_t>(block) * 2, 0.f);
    nodsynth::runtime::WavStream stream;
    std::string error;
    if (!stream.open(output, static_cast<std::uint32_t>(sampleRate), 2, error)) {
        std::cerr << error << '\n';
        return 4;
    }
    bool started = false;
    bool released = false;
    for (std::uint64_t rendered = 0; rendered < frames;) {
        const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(block, frames - rendered));
        std::vector<nodsynth::runtime::MidiEvent> events;
        if (!started) {
            events.push_back({0, nodsynth::runtime::MidiType::noteOn, 0, 69, 100});
            started = true;
        }
        if (!released && rendered <= noteOff && noteOff < rendered + chunk) {
            events.push_back({static_cast<std::uint32_t>(noteOff - rendered), nodsynth::runtime::MidiType::noteOff, 0, 69, 0});
            released = true;
        }
        float* outputs[2] = {left.data(), right.data()};
        engine.process(outputs, 2, chunk, events);
        for (std::uint32_t frame = 0; frame < chunk; ++frame) {
            interleaved[static_cast<std::size_t>(frame) * 2] = left[frame];
            interleaved[static_cast<std::size_t>(frame) * 2 + 1] = right[frame];
        }
        if (!stream.write(interleaved.data(), static_cast<std::size_t>(chunk) * 2, error)) {
            std::cerr << error << '\n';
            return 4;
        }
        rendered += chunk;
    }
    if (!stream.commit(error)) {
        std::cerr << error << '\n';
        return 4;
    }
    std::cout << "wrote " << frames << " frames\n";
    return 0;
}
