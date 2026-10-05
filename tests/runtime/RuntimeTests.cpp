#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/runtime/Engine.h>
#include <nodsynth/runtime/GraphSession.h>

using namespace nodsynth;
using Catch::Approx;

namespace {
model::NodeRecord node(std::string id, std::string type) {
    model::NodeRecord record;
    record.id = model::NodeId{std::move(id)};
    record.typeId = model::NodeTypeId{std::move(type)};
    record.schemaVersion = 1;
    record.opaqueStateJson = "{}";
    record.extensionsJson = "{}";
    return record;
}

void setParam(model::NodeRecord& record, std::string id, double value) {
    record.parameters[model::ParameterId{std::move(id)}] = value;
}

model::Connection cable(std::string from, std::string fromPort, std::string to, std::string toPort) {
    return {{model::NodeId{std::move(from)}, model::PortId{std::move(fromPort)}},
            {model::NodeId{std::move(to)}, model::PortId{std::move(toPort)}}};
}

runtime::EngineConfig configFor(std::uint32_t voices, double sampleRate, std::uint32_t frames) {
    runtime::EngineConfig config;
    config.audio.sampleRate = sampleRate;
    config.audio.maxFrames = frames;
    config.audio.voiceCount = voices;
    config.parameterSmoothSeconds = 0.0;
    config.releaseHoldSeconds = 0.05;
    return config;
}

struct Harness {
    runtime::Engine engine;
    model::SchemaRegistry registry{nodes::builtinRegistry()};
    std::vector<float> left;
    std::vector<float> right;

    explicit Harness(runtime::EngineConfig config) : engine(std::move(config)) {}

    bool open(const model::GraphSnapshot& graph) {
        const auto compiled = compiler::GraphCompiler{compiler::CompilerLimits{.maxVoices = engine.config().voiceCount}}.compile(
            graph, registry);
        if (!compiled.graph) return false;
        auto prepared = runtime::preparePlan(
            *compiled.graph, registry, nodes::builtinImplementations(), engine.config(), engine.voices(),
            engine.parameterSmoothSeconds());
        return prepared.plan && engine.stage(std::move(prepared.plan)).accepted;
    }

    void render(std::uint32_t frames, std::vector<runtime::MidiEvent> events = {}) {
        left.assign(frames, 0.f);
        right.assign(frames, 0.f);
        const auto chunk = engine.config().maxFrames;
        for (std::uint32_t offset = 0; offset < frames;) {
            const auto count = std::min(chunk, frames - offset);
            std::vector<runtime::MidiEvent> local;
            for (const auto& event : events) {
                if (event.sampleOffset < offset || event.sampleOffset >= offset + count) continue;
                auto copy = event;
                copy.sampleOffset -= offset;
                local.push_back(copy);
            }
            float* outputs[2] = {left.data() + offset, right.data() + offset};
            engine.process(outputs, 2, count, local);
            offset += count;
        }
    }

    const float* port(std::string nodeId, std::string portId, std::uint32_t voice = 0) const {
        const auto* plan = engine.activePlan();
        if (plan == nullptr) return nullptr;
        return plan->portSamples(model::NodeId{std::move(nodeId)}, model::PortId{std::move(portId)}, voice, 0);
    }
};

model::GraphSnapshot dcGraph() {
    auto osc = node("oscillator", "nod.oscillator");
    setParam(osc, "waveform", 3);
    setParam(osc, "level", 1);
    setParam(osc, "frequency", 0);
    auto mix = node("voice-mix", "nod.voice-mix");
    auto output = node("audio-output", "nod.audio-output");
    return {{osc, mix, output},
            {cable("oscillator", "audio", "voice-mix", "voices"), cable("voice-mix", "audio", "audio-output", "audio")}};
}

double frequencyOf(const std::vector<float>& samples, std::size_t begin, std::size_t end, double sampleRate) {
    int crossings = 0;
    for (std::size_t index = begin + 1; index < end; ++index) {
        const bool crossed = (samples[index - 1] <= 0.f && samples[index] > 0.f) ||
                             (samples[index - 1] >= 0.f && samples[index] < 0.f);
        if (crossed) ++crossings;
    }
    return 0.5 * crossings * sampleRate / static_cast<double>(end - begin - 1);
}
} // namespace

TEST_CASE("prepare rejects illegal runtime configurations") {
    runtime::Engine engine(configFor(1, 48000, 128));
    const auto registry = nodes::builtinRegistry();
    const auto compiled = compiler::GraphCompiler{compiler::CompilerLimits{.maxVoices = 1}}.compile(dcGraph(), registry);
    REQUIRE(compiled.graph);
    auto badRate = engine.config();
    badRate.sampleRate = 0;
    auto rejected = runtime::preparePlan(*compiled.graph, registry, nodes::builtinImplementations(), badRate, engine.voices(), 0);
    REQUIRE(rejected.error == runtime::PrepareError::invalidSampleRate);
    auto badBlock = engine.config();
    badBlock.maxFrames = 0;
    rejected = runtime::preparePlan(*compiled.graph, registry, nodes::builtinImplementations(), badBlock, engine.voices(), 0);
    REQUIRE(rejected.error == runtime::PrepareError::invalidBlockSize);
    auto mismatch = engine.config();
    mismatch.voiceCount = 2;
    rejected = runtime::preparePlan(*compiled.graph, registry, nodes::builtinImplementations(), mismatch, engine.voices(), 0);
    REQUIRE(rejected.error == runtime::PrepareError::voiceBudgetMismatch);
    auto tiny = engine.config();
    tiny.maxBytes = 32;
    rejected = runtime::preparePlan(*compiled.graph, registry, nodes::builtinImplementations(), tiny, engine.voices(), 0);
    REQUIRE(rejected.error == runtime::PrepareError::budgetExceeded);

    model::SchemaRegistry custom;
    model::NodeSchema schema;
    schema.typeId = model::NodeTypeId{"test.unknown"};
    schema.schemaVersion = 1;
    schema.displayName = "Unknown";
    schema.category = "Test";
    schema.sourceId = "test";
    schema.scope = model::NodeScope::global;
    schema.ports = {{model::PortId{"out"}, "Out", model::PortDirection::output, model::PortKind::audio, 1, model::PortDomain::sameAsNode}};
    REQUIRE(custom.registerSchema(std::move(schema)));
    const model::GraphSnapshot unknown{{node("mystery", "test.unknown")}, {}};
    const auto unknownCompiled = compiler::GraphCompiler{compiler::CompilerLimits{.maxVoices = 1}}.compile(unknown, custom);
    REQUIRE(unknownCompiled.graph);
    rejected = runtime::preparePlan(
        *unknownCompiled.graph, custom, nodes::builtinImplementations(), engine.config(), engine.voices(), 0);
    REQUIRE(rejected.error == runtime::PrepareError::unknownImplementation);
}

TEST_CASE("A4 is 440 Hz and a sine stays finite across sample rates") {
    for (const double sampleRate : {44100.0, 48000.0, 96000.0}) {
        Harness harness(configFor(1, sampleRate, 512));
        auto graph = nodes::sinePatch();
        for (auto& record : graph.nodes) {
            if (record.id.value == "envelope") {
                setParam(record, "attack", 0);
                setParam(record, "decay", 0);
                setParam(record, "sustain", 1);
            }
            if (record.id.value == "oscillator") setParam(record, "level", 0.5);
        }
        REQUIRE(harness.open(graph));
        harness.render(static_cast<std::uint32_t>(sampleRate * 0.2), {{0, runtime::MidiType::noteOn, 0, 69, 127}});
        for (float sample : harness.left) REQUIRE(std::isfinite(sample));
        const auto frequency = frequencyOf(harness.left, static_cast<std::size_t>(sampleRate * 0.05), harness.left.size(), sampleRate);
        REQUIRE(frequency == Approx(440.0).margin(1.0));
        const auto* hertz = harness.port("note-to-frequency", "frequency");
        REQUIRE(hertz != nullptr);
        REQUIRE(hertz[0] == Approx(440.f).margin(0.01f));
    }
}

TEST_CASE("square silence, gain, and voice mix follow the defined amplitudes") {
    Harness harness(configFor(1, 48000, 128));
    auto osc = node("oscillator", "nod.oscillator");
    setParam(osc, "waveform", 3);
    setParam(osc, "level", 1);
    setParam(osc, "frequency", 0);
    auto gain = node("gain", "nod.gain");
    auto amount = node("amount", "nod.scale-bias");
    setParam(amount, "bias", 0.5);
    auto mix = node("voice-mix", "nod.voice-mix");
    auto output = node("audio-output", "nod.audio-output");
    REQUIRE(harness.open(
        {{osc, gain, amount, mix, output},
         {cable("oscillator", "audio", "gain", "audio-in"), cable("amount", "output", "gain", "gain"),
          cable("gain", "audio-out", "voice-mix", "voices"), cable("voice-mix", "audio", "audio-output", "audio")}}));
    harness.render(128, {{0, runtime::MidiType::noteOn, 0, 60, 127}});
    REQUIRE(harness.left[10] == Approx(0.5f).margin(1.0e-4f));
    REQUIRE(harness.right[10] == Approx(harness.left[10]).margin(1.0e-5f));
}

TEST_CASE("control add, multiply, and scale-bias match their definitions") {
    Harness harness(configFor(1, 48000, 64));
    auto left = node("left", "nod.scale-bias");
    auto right = node("right", "nod.scale-bias");
    setParam(left, "bias", 0.25);
    setParam(right, "bias", 0.5);
    auto sum = node("sum", "nod.add");
    auto product = node("product", "nod.multiply");
    REQUIRE(harness.open({{left, right, sum, product},
                          {cable("left", "output", "sum", "a"), cable("right", "output", "sum", "b"),
                           cable("left", "output", "product", "a"), cable("right", "output", "product", "b")}}));
    harness.render(64);
    const auto* added = harness.port("sum", "sum");
    const auto* multiplied = harness.port("product", "product");
    REQUIRE(added[0] == Approx(0.75f).margin(1.0e-5f));
    REQUIRE(multiplied[0] == Approx(0.125f).margin(1.0e-5f));
}

TEST_CASE("ADSR stages land on the expected samples") {
    auto midi = node("midi", "nod.midi-input");
    auto envelope = node("envelope", "nod.adsr");
    setParam(envelope, "attack", 0.01);
    setParam(envelope, "decay", 0.01);
    setParam(envelope, "sustain", 0.5);
    setParam(envelope, "release", 0.01);
    Harness full(configFor(1, 48000, 2048));
    REQUIRE(full.open({{midi, envelope}, {cable("midi", "gate", "envelope", "gate")}}));
    full.render(2048, {{0, runtime::MidiType::noteOn, 0, 69, 127}, {1500, runtime::MidiType::noteOff, 0, 69, 0}});
    const auto* samples = full.port("envelope", "envelope");
    REQUIRE(samples[479] == Approx(1.f).margin(0.02f));
    REQUIRE(samples[959] == Approx(0.5f).margin(0.05f));
    REQUIRE(samples[1500 + 479] == Approx(0.f).margin(0.05f));
    REQUIRE(std::isfinite(samples[100]));
}

TEST_CASE("feedback delay reads history and ignores the current block") {
    Harness harness(configFor(1, 48000, 64));
    auto osc = node("oscillator", "nod.oscillator");
    setParam(osc, "waveform", 3);
    setParam(osc, "level", 1);
    setParam(osc, "frequency", 0);
    auto delay = node("delay", "nod.feedback-delay");
    setParam(delay, "time", 0);
    setParam(delay, "feedback", 0.5);
    auto mix = node("voice-mix", "nod.voice-mix");
    auto output = node("audio-output", "nod.audio-output");
    REQUIRE(harness.open(
        {{osc, delay, mix, output},
         {cable("oscillator", "audio", "delay", "audio-in"), cable("delay", "audio-out", "voice-mix", "voices"),
          cable("voice-mix", "audio", "audio-output", "audio")}}));
    harness.render(64, {{0, runtime::MidiType::noteOn, 0, 60, 127}});
    REQUIRE(harness.left[32] == Approx(0.f).margin(1.0e-4f));
    REQUIRE(harness.engine.setParameter(model::NodeId{"oscillator"}, model::ParameterId{"level"}, 0.f));
    harness.render(64);
    REQUIRE(harness.left[32] == Approx(1.f).margin(1.0e-3f));
    harness.render(64);
    REQUIRE(harness.left[32] == Approx(0.5f).margin(1.0e-3f));
    for (float sample : harness.left) REQUIRE(std::isfinite(sample));
}

TEST_CASE("rendering is repeatable across block sizes, including a short final block") {
    const auto renderAt = [](std::uint32_t maxFrames, std::uint32_t total) {
        Harness harness(configFor(1, 48000, maxFrames));
        auto graph = nodes::sinePatch();
        for (auto& record : graph.nodes) {
            if (record.id.value == "envelope") {
                setParam(record, "attack", 0);
                setParam(record, "decay", 0);
                setParam(record, "sustain", 1);
                setParam(record, "release", 0);
            }
        }
        REQUIRE(harness.open(graph));
        harness.render(total, {{0, runtime::MidiType::noteOn, 0, 69, 127}});
        return harness.left;
    };
    const auto block64 = renderAt(512, 100);
    const auto block100 = renderAt(128, 100);
    REQUIRE(block64.size() == block100.size());
    for (std::size_t index = 0; index < block64.size(); ++index) {
        REQUIRE(std::isfinite(block64[index]));
        REQUIRE(block64[index] == Approx(block100[index]).margin(1.0e-5f));
    }
}

TEST_CASE("parameter smoothing reaches the final value and a rejected plan keeps the old sound") {
    runtime::EngineConfig config = configFor(1, 48000, 128);
    config.parameterSmoothSeconds = 0.01;
    Harness harness(std::move(config));
    REQUIRE(harness.open(dcGraph()));
    harness.render(128, {{0, runtime::MidiType::noteOn, 0, 60, 127}});
    REQUIRE(harness.left[64] == Approx(1.f).margin(1.0e-3f));
    for (int value = 0; value < 300; ++value) {
        harness.engine.setParameter(model::NodeId{"oscillator"}, model::ParameterId{"level"}, value / 299.f);
    }
    harness.render(4800);
    REQUIRE(harness.left.back() == Approx(1.f).margin(0.05f));

    runtime::GraphSession session(configFor(1, 48000, 128), nodes::builtinRegistry(), nodes::builtinImplementations());
    session.editor().load(dcGraph());
    session.compileSync();
    REQUIRE(session.sounding());
    float keptLeft[128]{};
    float keptRight[128]{};
    float* keptOutputs[2] = {keptLeft, keptRight};
    session.engine().process(
        keptOutputs, 2, 128, std::vector<runtime::MidiEvent>{{0, runtime::MidiType::noteOn, 0, 60, 127}});
    REQUIRE(keptLeft[64] == Approx(1.f).margin(1.0e-3f));
    REQUIRE(session.editor().addNode(node("bad", "test.unknown")));
    REQUIRE(session.editor().connect(cable("bad", "out", "oscillator", "frequency")));
    session.compileSync();
    REQUIRE_FALSE(session.diagnostics().empty());
    REQUIRE(session.sounding());
    session.engine().process(keptOutputs, 2, 128, {});
    REQUIRE(keptLeft[64] == Approx(1.f).margin(1.0e-3f));
}

TEST_CASE("voice stealing prefers a releasing voice, then the oldest note") {
    runtime::VoiceAllocator voices;
    voices.prepare(2, 48000, 128, 1.0, 0.01);
    voices.renderBlock(std::vector<runtime::MidiEvent>{{0, runtime::MidiType::noteOn, 0, 60, 127},
                                                       {1, runtime::MidiType::noteOn, 0, 64, 127},
                                                       {2, runtime::MidiType::noteOff, 0, 60, 0},
                                                       {3, runtime::MidiType::noteOn, 0, 67, 127}},
                       8);
    REQUIRE(voices.status(0).note == 67);
    REQUIRE(voices.status(1).note == 64);
    REQUIRE(voices.attenuation(0)[3] == Approx(0.f).margin(1.0e-4f));
    voices.renderBlock(std::vector<runtime::MidiEvent>{{0, runtime::MidiType::noteOn, 1, 70, 100}}, 4);
    REQUIRE(voices.status(1).note == 70);
    REQUIRE(voices.status(1).channel == 1);
}

TEST_CASE("sustain and all-notes-off do not leave a stuck gate") {
    runtime::VoiceAllocator voices;
    voices.prepare(4, 48000, 32, 0.01, 0.01);
    voices.renderBlock(std::vector<runtime::MidiEvent>{{0, runtime::MidiType::noteOn, 0, 60, 127},
                                                       {1, runtime::MidiType::sustain, 0, 64, 127},
                                                       {2, runtime::MidiType::noteOff, 0, 60, 0}},
                       8);
    REQUIRE(voices.gate(0)[3] == 1.f);
    voices.renderBlock(std::vector<runtime::MidiEvent>{{0, runtime::MidiType::sustain, 0, 64, 0}}, 4);
    REQUIRE(voices.gate(0)[1] == 0.f);
    voices.renderBlock(std::vector<runtime::MidiEvent>{{0, runtime::MidiType::noteOn, 2, 62, 127},
                                                       {1, runtime::MidiType::allNotesOff, 2, 0, 0}},
                       4);
    for (std::uint32_t voice = 0; voice < 4; ++voice) REQUIRE(voices.gate(voice)[3] == 0.f);
}

TEST_CASE("lowpass attenuates a tone more when the cutoff is closed") {
    const auto energy = [](float cutoff) {
        Harness harness(configFor(1, 48000, 256));
        auto graph = nodes::sinePatch();
        graph.nodes.push_back(node("filter", "nod.lowpass"));
        setParam(graph.nodes.back(), "cutoff", cutoff);
        setParam(graph.nodes.back(), "resonance", 0);
        for (auto& record : graph.nodes) {
            if (record.id.value == "envelope") {
                setParam(record, "attack", 0);
                setParam(record, "decay", 0);
                setParam(record, "sustain", 1);
            }
        }
        std::erase_if(graph.connections, [](const model::Connection& connection) {
            return connection.from.nodeId.value == "oscillator" && connection.from.portId.value == "audio";
        });
        graph.connections.push_back(cable("oscillator", "audio", "filter", "audio-in"));
        graph.connections.push_back(cable("filter", "audio-out", "gain", "audio-in"));
        REQUIRE(harness.open(graph));
        harness.render(2048, {{0, runtime::MidiType::noteOn, 0, 69, 127}});
        double sum = 0;
        for (float sample : harness.left) {
            REQUIRE(std::isfinite(sample));
            sum += static_cast<double>(sample) * sample;
        }
        return sum;
    };
    REQUIRE(energy(20000) > energy(80) * 5.0);
}

TEST_CASE("sixteen voices stay finite for a sustained chord") {
    Harness harness(configFor(16, 48000, 128));
    REQUIRE(harness.open(nodes::sinePatch()));
    std::vector<runtime::MidiEvent> events;
    for (std::uint8_t note = 60; note < 76; ++note) events.push_back({0, runtime::MidiType::noteOn, 0, note, 100});
    harness.render(1024, events);
    for (float sample : harness.left) REQUIRE(std::isfinite(sample));
    REQUIRE(harness.engine.voices().activeCount() == 16);
}

TEST_CASE("plan crossfade stays finite and a failed second plan cannot exceed the peak budget") {
    runtime::EngineConfig config = configFor(1, 48000, 128);
    config.crossfadeSeconds = 0.015;
    config.maxPeakBytes = 64ull << 20;
    Harness harness(std::move(config));
    REQUIRE(harness.open(dcGraph()));
    harness.render(64, {{0, runtime::MidiType::noteOn, 0, 60, 127}});
    auto quiet = dcGraph();
    for (auto& record : quiet.nodes) {
        if (record.id.value == "oscillator") setParam(record, "level", 0);
    }
    const auto registry = nodes::builtinRegistry();
    const auto compiled = compiler::GraphCompiler{compiler::CompilerLimits{.maxVoices = 1}}.compile(quiet, registry);
    REQUIRE(compiled.graph);
    auto prepared = runtime::preparePlan(
        *compiled.graph, registry, nodes::builtinImplementations(), harness.engine.config(), harness.engine.voices(), 0);
    REQUIRE(prepared.plan);
    const auto bytes = prepared.plan->committedBytes();
    REQUIRE(harness.engine.stage(std::move(prepared.plan)).accepted);
    harness.render(2048);
    for (float sample : harness.left) REQUIRE(std::isfinite(sample));
    REQUIRE(harness.left.back() == Approx(0.f).margin(1.0e-3f));
    harness.engine.reclaim();
    auto again = runtime::preparePlan(
        *compiled.graph, registry, nodes::builtinImplementations(), harness.engine.config(), harness.engine.voices(), 0);
    runtime::Engine limited(configFor(1, 48000, 128));
    auto first = runtime::preparePlan(
        *compiled.graph, registry, nodes::builtinImplementations(), limited.config(), limited.voices(), 0);
    const auto planBytes = first.plan->committedBytes();
    REQUIRE(limited.stage(std::move(first.plan)).accepted);
    limited.process(nullptr, 0, 32, {});
    limited.setPeakBudget(limited.overheadBytes() + planBytes);
    auto second = runtime::preparePlan(
        *compiled.graph, registry, nodes::builtinImplementations(), limited.config(), limited.voices(), 0);
    REQUIRE(second.plan->committedBytes() + limited.overheadBytes() > limited.overheadBytes() + planBytes - 1);
    REQUIRE_FALSE(limited.stage(std::move(second.plan)).accepted);
    (void)bytes;
}

TEST_CASE("example patches stay finite and a stale compile cannot replace a newer edit") {
    Harness filter(configFor(4, 48000, 128));
    REQUIRE(filter.open(nodes::filterPatch()));
    filter.render(256, {{0, runtime::MidiType::noteOn, 0, 60, 100}});
    for (float sample : filter.left) REQUIRE(std::isfinite(sample));
    REQUIRE(std::fabs(filter.left[200]) > 0.001f);

    Harness delay(configFor(2, 48000, 128));
    REQUIRE(delay.open(nodes::delayPatch()));
    delay.render(1024, {{0, runtime::MidiType::noteOn, 0, 64, 110}});
    for (float sample : delay.left) REQUIRE(std::isfinite(sample));

    runtime::GraphSession session(configFor(16, 48000, 128), nodes::builtinRegistry(), nodes::builtinImplementations());
    session.editor().load(nodes::sinePatch());
    const auto firstRevision = session.editor().structureRevision();
    std::atomic<int> phase{0};
    std::atomic<int> entries{0};
    session.setTestHook([&] {
        if (entries.fetch_add(1) != 0) return;
        phase.store(1);
        while (phase.load() == 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    session.requestCompile();
    for (int spin = 0; spin < 2000 && phase.load() == 0; ++spin) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(phase.load() == 1);
    REQUIRE(session.editor().addNode(node("extra-gain", "nod.gain")));
    const auto latest = session.editor().structureRevision();
    REQUIRE(latest != firstRevision);
    session.requestCompile();
    phase.store(2);
    session.waitIdle();
    session.poll();
    REQUIRE(session.sounding());
    REQUIRE(session.soundingRevision() == latest);
}

TEST_CASE("pitch bend offsets the sounding MIDI note") {
    runtime::VoiceAllocator voices;
    voices.prepare(4, 48000.0, 16, 0.5, 0.01);
    const std::vector<runtime::MidiEvent> events{{0, runtime::MidiType::noteOn, 0, 69, 100}, {0, runtime::MidiType::pitchBend, 0, 127, 127}};
    voices.renderBlock(events, 4);
    const float expected = 69.f + (8191.f / 8192.f) * runtime::kPitchBendSemitones;
    REQUIRE(std::fabs(voices.note(0)[0] - expected) < 0.0001f);
}
