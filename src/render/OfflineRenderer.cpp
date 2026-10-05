#include <nodsynth/render/OfflineRenderer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <utility>

#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/persist/ProjectFile.h>
#include <nodsynth/runtime/Engine.h>
#include <nodsynth/runtime/WavFile.h>

namespace nodsynth::render {
namespace {
RenderReport fail(std::string code, std::string message, std::vector<midi::Diagnostic> diagnostics = {}) {
    RenderReport report;
    report.code = std::move(code);
    report.message = std::move(message);
    report.diagnostics = std::move(diagnostics);
    return report;
}

persist::Json diagnosticJson(const midi::Diagnostic& diagnostic) {
    persist::Json item = persist::Json::object();
    item.set("code", persist::Json::string(diagnostic.code));
    item.set("message", persist::Json::string(diagnostic.message));
    item.set("tick", persist::Json::number(diagnostic.tick));
    item.set("track", persist::Json::number(diagnostic.track));
    return item;
}

std::string hex64(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int index = 15; index >= 0; --index) {
        text[static_cast<std::size_t>(index)] = digits[value & 0xf];
        value >>= 4;
    }
    return text;
}

std::uint64_t mixHash(std::uint64_t hash, const float* samples, std::size_t count) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(samples);
    const auto size = count * sizeof(float);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool toEngine(const midi::Event& event, runtime::MidiEvent& midi) {
    midi.channel = event.channel;
    midi.data1 = event.data1;
    midi.data2 = event.data2;
    switch (event.kind) {
    case midi::EventKind::noteOn:
        midi.type = runtime::MidiType::noteOn;
        return true;
    case midi::EventKind::noteOff:
        midi.type = runtime::MidiType::noteOff;
        return true;
    case midi::EventKind::pitchBend:
        midi.type = runtime::MidiType::pitchBend;
        return true;
    case midi::EventKind::controlChange:
        if (event.data1 == 64) midi.type = runtime::MidiType::sustain;
        else if (event.data1 == 120) midi.type = runtime::MidiType::allSoundOff;
        else if (event.data1 == 123) midi.type = runtime::MidiType::allNotesOff;
        else return false;
        return true;
    default:
        return false;
    }
}

struct TimedEvent {
    std::int64_t sample{0};
    std::uint32_t order{0};
    runtime::MidiEvent midi{};
};

struct HeldNote {
    std::uint8_t channel{0};
    std::uint8_t note{0};
    int count{0};
};

void trackHeld(std::vector<HeldNote>& held, bool sustainDown[16], const midi::Event& event) {
    if (!event.hasChannel) return;
    if (event.kind == midi::EventKind::noteOn) {
        auto found = std::find_if(held.begin(), held.end(), [&](const HeldNote& note) {
            return note.channel == event.channel && note.note == event.data1;
        });
        if (found == held.end()) held.push_back({event.channel, event.data1, 1});
        else ++found->count;
    } else if (event.kind == midi::EventKind::noteOff) {
        auto found = std::find_if(held.begin(), held.end(), [&](const HeldNote& note) {
            return note.channel == event.channel && note.note == event.data1 && note.count > 0;
        });
        if (found != held.end()) --found->count;
    } else if (event.kind == midi::EventKind::controlChange && event.data1 == 64) {
        sustainDown[event.channel] = event.data2 >= 64;
    } else if (event.kind == midi::EventKind::controlChange && (event.data1 == 120 || event.data1 == 123)) {
        for (auto& note : held) {
            if (note.channel == event.channel) note.count = 0;
        }
        if (event.data1 == 120) sustainDown[event.channel] = false;
    }
}
} // namespace

persist::Json inspectMidi(const midi::File& file) {
    persist::Json json = persist::Json::object();
    json.set("format", persist::Json::number(file.format));
    json.set("ppq", persist::Json::number(file.ppq));
    json.set("tracks", persist::Json::number(file.trackCount));
    json.set("endTick", persist::Json::number(file.endTick));
    json.set("events", persist::Json::number(static_cast<double>(file.events.size())));
    json.set("explicitStreamRequired", persist::Json::boolean(midi::requiresExplicitStream(file)));
    persist::Json streams = persist::Json::array();
    for (const auto& stream : midi::noteStreams(file)) {
        persist::Json item = persist::Json::object();
        item.set("track", persist::Json::number(stream.track));
        item.set("channel", persist::Json::number(stream.channel));
        item.set("noteOns", persist::Json::number(stream.noteOns));
        streams.push(std::move(item));
    }
    json.set("streams", std::move(streams));
    persist::Json tempo = persist::Json::array();
    for (const auto& point : file.tempo) {
        persist::Json item = persist::Json::object();
        item.set("tick", persist::Json::number(point.tick));
        item.set("microsecondsPerQuarter", persist::Json::number(point.microsecondsPerQuarter));
        tempo.push(std::move(item));
    }
    json.set("tempo", std::move(tempo));
    const auto capability = midi::assess(file, midi::RenderMode::strict);
    json.set("renderable", persist::Json::boolean(!capability.rejected));
    persist::Json unsupported = persist::Json::array();
    for (const auto& diagnostic : capability.unsupported) unsupported.push(diagnosticJson(diagnostic));
    json.set("unsupported", std::move(unsupported));
    persist::Json diagnostics = persist::Json::array();
    for (const auto& diagnostic : file.diagnostics) diagnostics.push(diagnosticJson(diagnostic));
    json.set("diagnostics", std::move(diagnostics));
    return json;
}

persist::Json reportJson(const RenderReport& report, const midi::File* file) {
    persist::Json json = inspectMidi(file == nullptr ? midi::File{} : *file);
    json.set("status", persist::Json::string(report.ok ? "ok" : "rejected"));
    json.set("code", persist::Json::string(report.code));
    json.set("message", persist::Json::string(report.message));
    json.set("sampleRate", persist::Json::number(report.sampleRate));
    json.set("blockSize", persist::Json::number(report.blockSize));
    json.set("frames", persist::Json::number(static_cast<double>(report.frames)));
    json.set("tailFrames", persist::Json::number(static_cast<double>(report.tailFrames)));
    json.set("tailTruncated", persist::Json::boolean(report.tailTruncated));
    json.set("peak", persist::Json::number(report.peak));
    json.set("activeVoicesAtEnd", persist::Json::number(report.activeVoicesAtEnd));
    json.set("outputHash", persist::Json::string(report.outputHash));
    json.set("midiHash", persist::Json::string(report.midiHash));
    json.set("patchHash", persist::Json::string(report.patchHash));
    json.set("milliseconds", persist::Json::number(report.milliseconds));
    persist::Json warnings = persist::Json::array();
    for (const auto& diagnostic : report.diagnostics) warnings.push(diagnosticJson(diagnostic));
    json.set("warnings", std::move(warnings));
    return json;
}

std::string hashFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::uint64_t hash = 14695981039346656037ull;
    char buffer[4096];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const auto count = static_cast<std::size_t>(input.gcount());
        const auto* bytes = reinterpret_cast<const unsigned char*>(buffer);
        for (std::size_t index = 0; index < count; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    }
    return hex64(hash);
}

std::optional<model::GraphSnapshot> loadPatch(const std::filesystem::path& path, std::string& error) {
    auto document = persist::loadProject(path, error);
    if (!document) return std::nullopt;
    return std::move(document->graph);
}

RenderReport renderMidi(
    const model::GraphSnapshot& graph, const midi::File& file, const RenderOptions& options, const std::filesystem::path& output) {
    const auto started = std::chrono::steady_clock::now();
    auto finish = [&](RenderReport report) {
        report.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        report.sampleRate = static_cast<std::uint32_t>(options.sampleRate);
        report.blockSize = options.blockSize;
        return report;
    };
    if (!runtime::validSampleRate(options.sampleRate) || options.sampleRate != std::floor(options.sampleRate) ||
        !runtime::validFrameCount(options.blockSize) || options.tailSeconds < 0.0 || options.maxTailSeconds < 0.0 ||
        options.tailThreshold < 0.0 || options.maxEventsPerBlock == 0) {
        return finish(fail("invalid-options", "sample rate, block size, or tail is invalid"));
    }
    const auto rate = static_cast<std::uint32_t>(options.sampleRate);
    const auto capability = midi::assess(file, options.mode);
    if (capability.rejected) {
        return finish(fail("unsupported-event", "strict render rejected unsupported MIDI events", capability.unsupported));
    }
    const auto selectionChosen = options.selection.track.has_value() || options.selection.channel.has_value();
    if (midi::requiresExplicitStream(file) && !options.mergeChannels && !selectionChosen) {
        return finish(fail("stream-selection", "select a track or channel, or pass merge-channels for this single patch"));
    }

    const auto scheduled = midi::schedule(file, rate, options.blockSize, selectionChosen || options.mergeChannels ? options.selection : midi::Selection{});
    if (scheduled.status != midi::ParseStatus::ok) return finish(fail(scheduled.code, scheduled.message));

    std::vector<TimedEvent> timed;
    std::vector<HeldNote> held;
    bool sustainDown[16]{};
    std::uint32_t order = 0;
    for (const auto& playback : scheduled.events) {
        runtime::MidiEvent midi{};
        if (!toEngine(playback.source, midi)) continue;
        timed.push_back({playback.absoluteSample, order++, midi});
        trackHeld(held, sustainDown, playback.source);
    }
    const midi::TempoMap map(file);
    const auto mappedEnd = map.sampleAtTick(file.endTick, rate);
    if (!mappedEnd) return finish(fail("sample-overflow", "song length does not fit in a sample index"));
    std::int64_t endSample = *mappedEnd;
    for (const auto& event : timed) endSample = std::max(endSample, event.sample);
    for (const auto& note : held) {
        for (int count = 0; count < note.count; ++count) {
            timed.push_back({endSample, order++, {0, runtime::MidiType::noteOff, note.channel, note.note, 0}});
        }
    }
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        if (!sustainDown[channel]) continue;
        timed.push_back({endSample, order++, {0, runtime::MidiType::sustain, channel, 64, 0}});
    }
    std::stable_sort(timed.begin(), timed.end(), [](const TimedEvent& left, const TimedEvent& right) {
        if (left.sample != right.sample) return left.sample < right.sample;
        return left.order < right.order;
    });

    std::uint32_t blockEvents = 0;
    std::int64_t blockSample = -1;
    for (const auto& event : timed) {
        const auto blockStart = event.sample - event.sample % options.blockSize;
        if (blockStart != blockSample) {
            blockEvents = 0;
            blockSample = blockStart;
        }
        if (++blockEvents > options.maxEventsPerBlock) {
            return finish(fail("event-density", "a render block contains more MIDI events than the engine can play"));
        }
    }

    const auto budgetSeconds = options.tailMode == TailMode::fixed ? options.tailSeconds : options.maxTailSeconds;
    const auto tailBudget = static_cast<std::uint64_t>(std::llround(budgetSeconds * options.sampleRate));
    if (static_cast<std::uint64_t>(endSample) + tailBudget > (0xffffffffu - 36u) / 8u) {
        return finish(fail("riff-limit", "rendered audio exceeds the RIFF size limit"));
    }

    runtime::EngineConfig config;
    config.audio.sampleRate = options.sampleRate;
    config.audio.maxFrames = options.blockSize;
    config.audio.voiceCount = 16;
    config.releaseHoldSeconds = std::max(0.5, budgetSeconds);
    runtime::Engine engine(config);
    const auto registry = nodes::builtinRegistry();
    const auto compiled = compiler::GraphCompiler{}.compile(graph, registry);
    if (!compiled.graph) {
        const auto message = compiled.diagnostics.empty() ? "graph compile failed" : compiled.diagnostics.front().message;
        return finish(fail("compile-failed", message));
    }
    auto prepared = runtime::preparePlan(
        *compiled.graph, registry, nodes::builtinImplementations(), engine.config(), engine.voices(), engine.parameterSmoothSeconds());
    if (!prepared.plan || !engine.stage(std::move(prepared.plan)).accepted) {
        return finish(fail("prepare-failed", prepared.message.empty() ? "failed to prepare the patch" : prepared.message));
    }

    runtime::WavStream stream;
    std::string ioError;
    if (!stream.open(output, rate, 2, ioError)) return finish(fail("output-io", ioError));
    std::vector<float> left(options.blockSize, 0.f);
    std::vector<float> right(options.blockSize, 0.f);
    std::vector<float> interleaved(static_cast<std::size_t>(options.blockSize) * 2, 0.f);
    std::uint64_t hash = 14695981039346656037ull;
    float peak = 0.f;
    std::size_t cursor = 0;
    const auto renderChunk = [&](std::uint64_t rendered, std::uint32_t chunk) {
        std::vector<runtime::MidiEvent> block;
        const auto blockEnd = static_cast<std::int64_t>(rendered + chunk);
        while (cursor < timed.size() && timed[cursor].sample < blockEnd) {
            if (timed[cursor].sample >= static_cast<std::int64_t>(rendered)) {
                auto event = timed[cursor].midi;
                event.sampleOffset = static_cast<std::uint32_t>(timed[cursor].sample - static_cast<std::int64_t>(rendered));
                block.push_back(event);
            }
            ++cursor;
        }
        float* outputs[] = {left.data(), right.data()};
        engine.process(outputs, 2, chunk, block);
        engine.reclaim();
        float blockPeak = 0.f;
        for (std::uint32_t frame = 0; frame < chunk; ++frame) {
            interleaved[static_cast<std::size_t>(frame) * 2] = left[frame];
            interleaved[static_cast<std::size_t>(frame) * 2 + 1] = right[frame];
            blockPeak = std::max(blockPeak, std::max(std::fabs(left[frame]), std::fabs(right[frame])));
        }
        peak = std::max(peak, blockPeak);
        hash = mixHash(hash, interleaved.data(), static_cast<std::size_t>(chunk) * 2);
        return blockPeak;
    };

    std::uint64_t rendered = 0;
    const auto musicalFrames = static_cast<std::uint64_t>(endSample);
    while (rendered < musicalFrames) {
        const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.blockSize, musicalFrames - rendered));
        renderChunk(rendered, chunk);
        if (!stream.write(interleaved.data(), static_cast<std::size_t>(chunk) * 2, ioError)) return finish(fail("output-io", ioError));
        rendered += chunk;
    }

    std::uint64_t tailFrames = 0;
    bool tailTruncated = false;
    if (options.tailMode == TailMode::fixed) {
        while (tailFrames < tailBudget) {
            const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.blockSize, tailBudget - tailFrames));
            renderChunk(rendered, chunk);
            if (!stream.write(interleaved.data(), static_cast<std::size_t>(chunk) * 2, ioError)) return finish(fail("output-io", ioError));
            rendered += chunk;
            tailFrames += chunk;
        }
    } else {
        bool hot = true;
        while (tailFrames < tailBudget && hot) {
            const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.blockSize, tailBudget - tailFrames));
            hot = renderChunk(rendered, chunk) >= static_cast<float>(options.tailThreshold);
            if (!stream.write(interleaved.data(), static_cast<std::size_t>(chunk) * 2, ioError)) return finish(fail("output-io", ioError));
            rendered += chunk;
            tailFrames += chunk;
        }
        tailTruncated = hot && tailFrames >= tailBudget;
    }
    if (!stream.commit(ioError)) return finish(fail("output-io", ioError));

    RenderReport report;
    report.ok = true;
    report.frames = rendered;
    report.tailFrames = tailFrames;
    report.tailTruncated = tailTruncated;
    report.peak = peak;
    report.activeVoicesAtEnd = engine.telemetry().activeVoices;
    report.outputHash = hex64(hash);
    report.midiHash = options.midiHash;
    report.patchHash = options.patchHash;
    report.diagnostics = file.diagnostics;
    if (options.mode == midi::RenderMode::loose) {
        report.diagnostics.insert(report.diagnostics.end(), capability.unsupported.begin(), capability.unsupported.end());
        report.diagnostics.insert(report.diagnostics.end(), capability.preserved.begin(), capability.preserved.end());
    }
    report.message = "rendered";
    return finish(std::move(report));
}
} // namespace nodsynth::render
