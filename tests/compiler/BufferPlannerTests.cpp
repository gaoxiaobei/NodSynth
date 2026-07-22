#include <catch2/catch_test_macros.hpp>
#include <nodsynth/compiler/GraphCompiler.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace nodsynth::compiler;
using namespace nodsynth::model;

namespace {
NodeRecord node(std::string id, std::string type) {
    return {NodeId{std::move(id)}, NodeTypeId{std::move(type)}, 1, {}, "{}", {}};
}

PortSchema port(
    std::string id,
    PortDirection direction,
    PortKind kind,
    std::uint32_t channels = 1,
    PortDomain domain = PortDomain::sameAsNode) {
    return {PortId{std::move(id)}, "Port", direction, kind, channels, domain};
}

NodeSchema schema(
    std::string type,
    NodeScope scope,
    std::vector<PortSchema> ports,
    bool breaksDependencyCycle = false) {
    return {
        NodeTypeId{std::move(type)}, 1, "Node", "Test", "test", scope, std::move(ports), {},
        breaksDependencyCycle,
    };
}

Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort) {
    return {
        {NodeId{std::move(fromNode)}, PortId{std::move(fromPort)}},
        {NodeId{std::move(toNode)}, PortId{std::move(toPort)}},
    };
}

bool hasDiagnostic(const CompileResult& result, DiagnosticCode code) {
    return std::ranges::any_of(result.diagnostics, [code](const Diagnostic& diagnostic) {
        return diagnostic.code == code;
    });
}

const BufferAssignment& audioAssignmentFor(const CompiledGraph& graph, const Endpoint& endpoint) {
    const auto found = std::ranges::find(graph.audioBuffers, endpoint, &BufferAssignment::output);
    if (found == graph.audioBuffers.end()) throw std::logic_error{"missing test audio buffer assignment"};
    return *found;
}

const BufferAssignment& controlAssignmentFor(const CompiledGraph& graph, const Endpoint& endpoint) {
    const auto found = std::ranges::find(graph.controlBuffers, endpoint, &BufferAssignment::output);
    if (found == graph.controlBuffers.end()) {
        throw std::logic_error{"missing test control buffer assignment"};
    }
    return *found;
}

std::pair<SchemaRegistry, GraphSnapshot> linearAudioGraphWithTwoNonOverlappingTemps() {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.source", NodeScope::global,
        {port("audio", PortDirection::output, PortKind::audio)})));
    REQUIRE(registry.registerSchema(schema(
        "test.process", NodeScope::global,
        {port("in", PortDirection::input, PortKind::audio),
         port("audio", PortDirection::output, PortKind::audio)})));
    REQUIRE(registry.registerSchema(schema(
        "test.sink", NodeScope::global,
        {port("in", PortDirection::input, PortKind::audio)})));
    return {
        std::move(registry),
        GraphSnapshot{
            {
                node("temp-b", "test.process"),
                node("source-b", "test.process"),
                node("sink", "test.sink"),
                node("source-a", "test.source"),
                node("temp-a", "test.process"),
            },
            {
                connection("temp-b", "audio", "sink", "in"),
                connection("source-a", "audio", "temp-a", "in"),
                connection("source-b", "audio", "temp-b", "in"),
                connection("temp-a", "audio", "source-b", "in"),
            },
        },
    };
}

std::pair<SchemaRegistry, GraphSnapshot> wideAudioGraph(NodeScope scope = NodeScope::global) {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.wide-source", scope,
        {port("audio", PortDirection::output, PortKind::audio)})));
    REQUIRE(registry.registerSchema(schema(
        "test.wide-sink", scope,
        {
            port("a", PortDirection::input, PortKind::audio),
            port("b", PortDirection::input, PortKind::audio),
            port("c", PortDirection::input, PortKind::audio),
        })));
    return {
        std::move(registry),
        GraphSnapshot{
            {
                node("source-c", "test.wide-source"),
                node("sink", "test.wide-sink"),
                node("source-a", "test.wide-source"),
                node("source-b", "test.wide-source"),
            },
            {
                connection("source-c", "audio", "sink", "c"),
                connection("source-a", "audio", "sink", "a"),
                connection("source-b", "audio", "sink", "b"),
            },
        },
    };
}

std::vector<std::pair<Endpoint, std::uint32_t>> assignmentSnapshot(
    const std::vector<BufferAssignment>& assignments) {
    std::vector<std::pair<Endpoint, std::uint32_t>> snapshot;
    for (const auto& assignment : assignments) snapshot.emplace_back(assignment.output, assignment.slot);
    return snapshot;
}
} // namespace

TEST_CASE("audio buffer slots are reused after the last consumer") {
    auto [registry, graph] = linearAudioGraphWithTwoNonOverlappingTemps();

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->audioBuffers.size() == 4);
    REQUIRE(audioAssignmentFor(*result.graph, {NodeId{"source-a"}, PortId{"audio"}}).slot ==
            audioAssignmentFor(*result.graph, {NodeId{"source-b"}, PortId{"audio"}}).slot);
}

TEST_CASE("audio slots only reuse buffers with the same channel count") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.mono", NodeScope::global,
        {port("out", PortDirection::output, PortKind::audio)})));
    REQUIRE(registry.registerSchema(schema(
        "test.stereo-process", NodeScope::global,
        {port("in", PortDirection::input, PortKind::audio),
         port("out", PortDirection::output, PortKind::audio, 2)})));
    REQUIRE(registry.registerSchema(schema(
        "test.stereo-sink", NodeScope::global,
        {port("in", PortDirection::input, PortKind::audio, 2)})));
    const GraphSnapshot graph{
        {node("mono", "test.mono"), node("stereo", "test.stereo-process"),
         node("sink", "test.stereo-sink")},
        {connection("mono", "out", "stereo", "in"), connection("stereo", "out", "sink", "in")},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    REQUIRE(audioAssignmentFor(*result.graph, {NodeId{"mono"}, PortId{"out"}}).slot !=
            audioAssignmentFor(*result.graph, {NodeId{"stereo"}, PortId{"out"}}).slot);
}

TEST_CASE("audio and control buffers use separate scope namespaces") {
    SchemaRegistry registry;
    for (const auto scope : {NodeScope::perVoice, NodeScope::global}) {
        const auto suffix = scope == NodeScope::perVoice ? "voice" : "global";
        REQUIRE(registry.registerSchema(schema(
            std::string{"test.audio-"} + suffix, scope,
            {port("audio", PortDirection::output, PortKind::audio)})));
        REQUIRE(registry.registerSchema(schema(
            std::string{"test.control-"} + suffix, scope,
            {port("control", PortDirection::output, PortKind::control)})));
    }
    const GraphSnapshot graph{
        {
            node("voice-audio", "test.audio-voice"),
            node("global-control", "test.control-global"),
            node("global-audio", "test.audio-global"),
            node("voice-control", "test.control-voice"),
        },
        {},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    REQUIRE(audioAssignmentFor(*result.graph, {NodeId{"voice-audio"}, PortId{"audio"}}).slot == 0);
    REQUIRE(audioAssignmentFor(*result.graph, {NodeId{"global-audio"}, PortId{"audio"}}).slot == 0);
    REQUIRE(controlAssignmentFor(*result.graph, {NodeId{"voice-control"}, PortId{"control"}}).slot == 0);
    REQUIRE(controlAssignmentFor(*result.graph, {NodeId{"global-control"}, PortId{"control"}}).slot == 0);
}

TEST_CASE("voice mix preserves per-voice input and allocates its stereo output globally") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.voice-source", NodeScope::perVoice,
        {port("out", PortDirection::output, PortKind::audio)})));
    REQUIRE(registry.registerSchema(schema(
        "test.voice-mix", NodeScope::global,
        {port("voices", PortDirection::input, PortKind::audio, 1, PortDomain::perVoice),
         port("out", PortDirection::output, PortKind::audio, 2)})));
    REQUIRE(registry.registerSchema(schema(
        "test.audio-output", NodeScope::global,
        {port("in", PortDirection::input, PortKind::audio, 2)})));
    const GraphSnapshot graph{
        {
            node("source", "test.voice-source"),
            node("mix", "test.voice-mix"),
            node("output", "test.audio-output"),
        },
        {
            connection("source", "out", "mix", "voices"),
            connection("mix", "out", "output", "in"),
        },
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    const auto& voice = audioAssignmentFor(*result.graph, {NodeId{"source"}, PortId{"out"}});
    const auto& mixed = audioAssignmentFor(*result.graph, {NodeId{"mix"}, PortId{"out"}});
    REQUIRE(voice.domain == NodeScope::perVoice);
    REQUIRE(voice.channels == 1);
    REQUIRE(mixed.domain == NodeScope::global);
    REQUIRE(mixed.channels == 2);
}

TEST_CASE("event connections use slot zero without sample buffer assignments") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.events", NodeScope::global,
        {port("gate", PortDirection::output, PortKind::gate),
         port("note", PortDirection::output, PortKind::note)})));
    REQUIRE(registry.registerSchema(schema(
        "test.event-sink", NodeScope::global,
        {port("gate", PortDirection::input, PortKind::gate),
         port("note", PortDirection::input, PortKind::note)})));
    const GraphSnapshot graph{
        {node("events", "test.events"), node("sink", "test.event-sink")},
        {connection("events", "note", "sink", "note"),
         connection("events", "gate", "sink", "gate")},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->audioBuffers.empty());
    REQUIRE(result.graph->controlBuffers.empty());
    REQUIRE(result.graph->connections.size() == 2);
    REQUIRE(std::ranges::all_of(result.graph->connections, [](const CompiledGraph::Connection& item) {
        return item.bufferSlot == 0;
    }));
}

TEST_CASE("compiled records and assignments are independent of insertion order") {
    auto [registry, graph] = linearAudioGraphWithTwoNonOverlappingTemps();
    const auto baseline = GraphCompiler{}.compile(graph, registry);
    REQUIRE(baseline.graph.has_value());

    std::ranges::reverse(graph.nodes);
    std::ranges::reverse(graph.connections);
    const auto permuted = GraphCompiler{}.compile(graph, registry);

    REQUIRE(permuted.graph.has_value());
    REQUIRE(assignmentSnapshot(permuted.graph->audioBuffers) ==
            assignmentSnapshot(baseline.graph->audioBuffers));
    REQUIRE(std::ranges::is_sorted(
        permuted.graph->nodes, {}, [](const CompiledGraph::Node& item) { return item.record.id; }));
    REQUIRE(std::ranges::is_sorted(
        permuted.graph->connections, {},
        [](const CompiledGraph::Connection& item) { return item.record; }));
}

TEST_CASE("audio and control connections carry their producer assignment") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.signal-source", NodeScope::global,
        {port("audio", PortDirection::output, PortKind::audio),
         port("control", PortDirection::output, PortKind::control)})));
    REQUIRE(registry.registerSchema(schema(
        "test.signal-sink", NodeScope::global,
        {port("audio", PortDirection::input, PortKind::audio),
         port("control", PortDirection::input, PortKind::control)})));
    const GraphSnapshot graph{
        {node("source", "test.signal-source"), node("sink", "test.signal-sink")},
        {connection("source", "control", "sink", "control"),
         connection("source", "audio", "sink", "audio")},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    for (const auto& compiledConnection : result.graph->connections) {
        const auto expectedSlot = compiledConnection.kind == PortKind::audio
                                      ? audioAssignmentFor(*result.graph, compiledConnection.record.from).slot
                                      : controlAssignmentFor(*result.graph, compiledConnection.record.from).slot;
        REQUIRE(compiledConnection.bufferSlot == expectedSlot);
    }
}

TEST_CASE("compiler rejects a plan above its global buffer budget") {
    auto [registry, graph] = wideAudioGraph();

    const auto result = GraphCompiler{{.maxNodes = 4096, .maxPhysicalBufferChannels = 2, .maxVoices = 16}}
                            .compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::resourceLimitExceeded));
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics.front().message.find("3") != std::string::npos);
    REQUIRE(result.diagnostics.front().message.find("2") != std::string::npos);
    REQUIRE_FALSE(result.diagnostics.front().nodeId.has_value());
    REQUIRE_FALSE(result.diagnostics.front().portId.has_value());
}

TEST_CASE("compiler multiplies per-voice buffers by the default sixteen voices") {
    auto [registry, graph] = wideAudioGraph(NodeScope::perVoice);

    const auto result = GraphCompiler{{.maxNodes = 4096, .maxPhysicalBufferChannels = 47, .maxVoices = 16}}
                            .compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics.front().code == DiagnosticCode::resourceLimitExceeded);
    REQUIRE(result.diagnostics.front().message.find("48") != std::string::npos);
    REQUIRE(result.diagnostics.front().message.find("47") != std::string::npos);
}

TEST_CASE("compiler rejects the node limit before endpoint validation") {
    SchemaRegistry registry;
    const GraphSnapshot graph{
        {node("missing-type", "test.absent")},
        {connection("missing-source", "out", "missing-sink", "in")},
    };

    const auto result = GraphCompiler{{.maxNodes = 0, .maxPhysicalBufferChannels = 8192, .maxVoices = 16}}
                            .compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics.front().code == DiagnosticCode::resourceLimitExceeded);
    REQUIRE(result.diagnostics.front().message.find("1") != std::string::npos);
    REQUIRE(result.diagnostics.front().message.find("0") != std::string::npos);
    REQUIRE_FALSE(result.diagnostics.front().nodeId.has_value());
    REQUIRE_FALSE(result.diagnostics.front().portId.has_value());
}

TEST_CASE("latency-break back-edge input keeps its producer buffer through the schedule") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.latency-input", NodeScope::global,
        {port("in", PortDirection::input, PortKind::audio)}, true)));
    REQUIRE(registry.registerSchema(schema(
        "test.audio-output", NodeScope::global,
        {port("out", PortDirection::output, PortKind::audio)})));
    const GraphSnapshot graph{
        {
            node("z-later", "test.audio-output"),
            node("producer", "test.audio-output"),
            node("delay", "test.latency-input"),
        },
        {connection("producer", "out", "delay", "in")},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->globalOrder ==
            std::vector<NodeId>{NodeId{"delay"}, NodeId{"producer"}, NodeId{"z-later"}});
    REQUIRE(audioAssignmentFor(*result.graph, {NodeId{"producer"}, PortId{"out"}}).slot !=
            audioAssignmentFor(*result.graph, {NodeId{"z-later"}, PortId{"out"}}).slot);
}

TEST_CASE("self latency-break input keeps its control buffer through the schedule") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.control-delay", NodeScope::global,
        {port("in", PortDirection::input, PortKind::control),
         port("out", PortDirection::output, PortKind::control)},
        true)));
    REQUIRE(registry.registerSchema(schema(
        "test.control-output", NodeScope::global,
        {port("out", PortDirection::output, PortKind::control)})));
    const GraphSnapshot graph{
        {node("z-later", "test.control-output"), node("delay", "test.control-delay")},
        {connection("delay", "out", "delay", "in")},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->globalOrder == std::vector<NodeId>{NodeId{"delay"}, NodeId{"z-later"}});
    REQUIRE(controlAssignmentFor(*result.graph, {NodeId{"delay"}, PortId{"out"}}).slot !=
            controlAssignmentFor(*result.graph, {NodeId{"z-later"}, PortId{"out"}}).slot);
}

TEST_CASE("compiler rejects physical channel usage above the largest configured limit") {
    constexpr auto maximumChannels = std::numeric_limits<std::uint32_t>::max();
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.maximum-source", NodeScope::global,
        {port("out", PortDirection::output, PortKind::audio, maximumChannels)})));
    REQUIRE(registry.registerSchema(schema(
        "test.maximum-sink", NodeScope::global,
        {port("a", PortDirection::input, PortKind::audio, maximumChannels),
         port("b", PortDirection::input, PortKind::audio, maximumChannels)})));
    const GraphSnapshot graph{
        {
            node("source-b", "test.maximum-source"),
            node("sink", "test.maximum-sink"),
            node("source-a", "test.maximum-source"),
        },
        {
            connection("source-a", "out", "sink", "a"),
            connection("source-b", "out", "sink", "b"),
        },
    };

    const auto result = GraphCompiler{{
        .maxNodes = 4096,
        .maxPhysicalBufferChannels = maximumChannels,
        .maxVoices = 16,
    }}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics.front().code == DiagnosticCode::resourceLimitExceeded);
    REQUIRE(result.diagnostics.front().message.find("8589934590") != std::string::npos);
    REQUIRE(result.diagnostics.front().message.find("4294967295") != std::string::npos);
}

TEST_CASE("latency-break back-edge output does not inherit an expired audio slot") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.audio-source", NodeScope::global,
        {port("out", PortDirection::output, PortKind::audio)})));
    REQUIRE(registry.registerSchema(schema(
        "test.audio-sink", NodeScope::global,
        {port("in", PortDirection::input, PortKind::audio)})));
    REQUIRE(registry.registerSchema(schema(
        "test.audio-delay", NodeScope::global,
        {port("in", PortDirection::input, PortKind::audio)}, true)));
    const GraphSnapshot graph{
        {
            node("d-producer", "test.audio-source"),
            node("c-delay", "test.audio-delay"),
            node("b-temp-consumer", "test.audio-sink"),
            node("a-temp-producer", "test.audio-source"),
        },
        {
            connection("d-producer", "out", "c-delay", "in"),
            connection("a-temp-producer", "out", "b-temp-consumer", "in"),
        },
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->globalOrder ==
            std::vector<NodeId>{NodeId{"a-temp-producer"}, NodeId{"b-temp-consumer"},
                                NodeId{"c-delay"}, NodeId{"d-producer"}});
    REQUIRE(audioAssignmentFor(*result.graph, {NodeId{"a-temp-producer"}, PortId{"out"}}).slot !=
            audioAssignmentFor(*result.graph, {NodeId{"d-producer"}, PortId{"out"}}).slot);
}

TEST_CASE("self latency-break output does not inherit an expired control slot") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schema(
        "test.control-source", NodeScope::global,
        {port("out", PortDirection::output, PortKind::control)})));
    REQUIRE(registry.registerSchema(schema(
        "test.control-sink", NodeScope::global,
        {port("in", PortDirection::input, PortKind::control)})));
    REQUIRE(registry.registerSchema(schema(
        "test.control-delay", NodeScope::global,
        {port("in", PortDirection::input, PortKind::control),
         port("out", PortDirection::output, PortKind::control)},
        true)));
    const GraphSnapshot graph{
        {
            node("c-delay", "test.control-delay"),
            node("b-temp-consumer", "test.control-sink"),
            node("a-temp-producer", "test.control-source"),
        },
        {
            connection("c-delay", "out", "c-delay", "in"),
            connection("a-temp-producer", "out", "b-temp-consumer", "in"),
        },
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->globalOrder ==
            std::vector<NodeId>{NodeId{"a-temp-producer"}, NodeId{"b-temp-consumer"}, NodeId{"c-delay"}});
    REQUIRE(controlAssignmentFor(*result.graph, {NodeId{"a-temp-producer"}, PortId{"out"}}).slot !=
            controlAssignmentFor(*result.graph, {NodeId{"c-delay"}, PortId{"out"}}).slot);
}
