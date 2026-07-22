#include <catch2/catch_test_macros.hpp>
#include <nodsynth/compiler/GraphCompiler.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace nodsynth::model;
using namespace nodsynth::compiler;

namespace {
NodeSchema processingSchema(
    std::string type,
    NodeScope scope,
    PortDomain inputDomain = PortDomain::sameAsNode,
    bool breaksDependencyCycle = false) {
    return {
        .typeId = NodeTypeId{std::move(type)},
        .schemaVersion = 1,
        .displayName = "Processing Node",
        .category = "Test",
        .sourceId = "test",
        .scope = scope,
        .ports = {
            {PortId{"in"}, "In", PortDirection::input, PortKind::audio, 1, inputDomain},
            {PortId{"out"}, "Out", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = breaksDependencyCycle,
    };
}

NodeSchema sourceSchema(std::string type, NodeScope scope) {
    auto schema = processingSchema(std::move(type), scope);
    schema.ports.erase(schema.ports.begin());
    return schema;
}

NodeSchema sinkSchema(std::string type, NodeScope scope) {
    auto schema = processingSchema(std::move(type), scope);
    schema.ports.pop_back();
    return schema;
}

NodeRecord node(std::string id, std::string type) {
    return {NodeId{std::move(id)}, NodeTypeId{std::move(type)}, 1, {}, "{}", {}};
}

Connection connection(std::string from, std::string to) {
    return {{NodeId{std::move(from)}, PortId{"out"}}, {NodeId{std::move(to)}, PortId{"in"}}};
}

bool hasDiagnostic(const CompileResult& result, DiagnosticCode code) {
    return std::ranges::any_of(result.diagnostics, [code](const Diagnostic& diagnostic) {
        return diagnostic.code == code;
    });
}

std::pair<SchemaRegistry, GraphSnapshot> graphWithIllegalScopeCrossing() {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(sourceSchema("test.voice-source", NodeScope::perVoice)));
    REQUIRE(registry.registerSchema(sinkSchema("test.global-sink", NodeScope::global)));
    return {
        std::move(registry),
        GraphSnapshot{
            {node("osc", "test.voice-source"), node("out", "test.global-sink")},
            {connection("osc", "out")},
        },
    };
}

std::pair<SchemaRegistry, GraphSnapshot> graphWithVoiceMix() {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(sourceSchema("test.voice-source", NodeScope::perVoice)));
    REQUIRE(registry.registerSchema(
        processingSchema("test.voice-mix", NodeScope::global, PortDomain::perVoice)));
    REQUIRE(registry.registerSchema(sinkSchema("test.global-sink", NodeScope::global)));
    return {
        std::move(registry),
        GraphSnapshot{
            {
                node("out", "test.global-sink"),
                node("osc", "test.voice-source"),
                node("mix", "test.voice-mix"),
            },
            {connection("mix", "out"), connection("osc", "mix")},
        },
    };
}

std::pair<SchemaRegistry, GraphSnapshot> graphWithGainCycle(bool latencyBreak) {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(processingSchema("test.gain", NodeScope::global)));
    REQUIRE(registry.registerSchema(
        processingSchema("test.delay", NodeScope::global, PortDomain::sameAsNode, latencyBreak)));
    REQUIRE(registry.registerSchema(sinkSchema("test.global-sink", NodeScope::global)));
    return {
        std::move(registry),
        GraphSnapshot{
            {
                node("out", "test.global-sink"),
                node("gain", "test.gain"),
                node("delay", "test.delay"),
            },
            {
                connection("gain", "out"),
                connection("gain", "delay"),
                connection("delay", "gain"),
            },
        },
    };
}

std::pair<SchemaRegistry, GraphSnapshot> graphForDeterministicScheduling() {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(sourceSchema("test.voice-source", NodeScope::perVoice)));
    REQUIRE(registry.registerSchema(processingSchema("test.voice-gain", NodeScope::perVoice)));
    REQUIRE(registry.registerSchema(
        processingSchema("test.voice-mix", NodeScope::global, PortDomain::perVoice)));
    REQUIRE(registry.registerSchema(sourceSchema("test.global-source", NodeScope::global)));
    REQUIRE(registry.registerSchema(sinkSchema("test.global-sink", NodeScope::global)));
    return {
        std::move(registry),
        GraphSnapshot{
            {
                node("voice-z", "test.voice-source"),
                node("global-a", "test.global-source"),
                node("out", "test.global-sink"),
                node("voice-gain", "test.voice-gain"),
                node("mix", "test.voice-mix"),
                node("voice-a", "test.voice-source"),
            },
            {
                connection("mix", "out"),
                connection("voice-gain", "mix"),
                connection("voice-z", "voice-gain"),
            },
        },
    };
}
} // namespace

TEST_CASE("per-voice output cannot cross scope into an ordinary global input") {
    auto [registry, graph] = graphWithIllegalScopeCrossing();

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::domainMismatch));
}

TEST_CASE("global output cannot feed a per-voice input") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(sourceSchema("test.global-source", NodeScope::global)));
    REQUIRE(registry.registerSchema(sinkSchema("test.voice-sink", NodeScope::perVoice)));
    const GraphSnapshot graph{
        {node("global", "test.global-source"), node("voice", "test.voice-sink")},
        {connection("global", "voice")},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::domainMismatch));
}

TEST_CASE("voice boundary accepts per-voice input and produces global output") {
    auto [registry, graph] = graphWithVoiceMix();

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->perVoiceOrder == std::vector<NodeId>{NodeId{"osc"}});
    REQUIRE(result.graph->globalOrder == std::vector<NodeId>{NodeId{"mix"}, NodeId{"out"}});
}

TEST_CASE("ordinary feedback is rejected") {
    auto [registry, graph] = graphWithGainCycle(false);

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::cycleDetected));
}

TEST_CASE("latency break makes feedback schedulable") {
    auto [registry, graph] = graphWithGainCycle(true);

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->globalOrder ==
            std::vector<NodeId>{NodeId{"delay"}, NodeId{"gain"}, NodeId{"out"}});
}

TEST_CASE("scheduling is deterministic across insertion order") {
    auto [registry, graph] = graphForDeterministicScheduling();
    const auto baseline = GraphCompiler{}.compile(graph, registry);
    REQUIRE(baseline.graph.has_value());
    REQUIRE(baseline.graph->perVoiceOrder ==
            std::vector<NodeId>{NodeId{"voice-a"}, NodeId{"voice-z"}, NodeId{"voice-gain"}});
    REQUIRE(baseline.graph->globalOrder ==
            std::vector<NodeId>{NodeId{"global-a"}, NodeId{"mix"}, NodeId{"out"}});

    for (std::size_t permutation = 0; permutation < 10; ++permutation) {
        auto permuted = graph;
        std::ranges::rotate(permuted.nodes, permuted.nodes.begin() + permutation % permuted.nodes.size());
        std::ranges::rotate(
            permuted.connections,
            permuted.connections.begin() + permutation % permuted.connections.size());
        if (permutation % 2 != 0) {
            std::ranges::reverse(permuted.nodes);
            std::ranges::reverse(permuted.connections);
        }

        const auto result = GraphCompiler{}.compile(permuted, registry);
        REQUIRE(result.graph.has_value());
        REQUIRE(result.graph->perVoiceOrder == baseline.graph->perVoiceOrder);
        REQUIRE(result.graph->globalOrder == baseline.graph->globalOrder);
    }
}
