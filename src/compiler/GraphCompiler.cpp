#include <nodsynth/compiler/GraphCompiler.h>

#include "BufferPlanner.h"
#include "DependencyGraph.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace nodsynth::compiler {
namespace {
using NodeRecords = std::map<model::NodeId, std::vector<const model::NodeRecord*>>;
using NodeSchemas = std::map<model::NodeId, const model::NodeSchema*>;

struct ResolvedEndpoint {
    const model::NodeSchema& nodeSchema;
    const model::PortSchema& portSchema;
};

model::NodeScope resolveDomain(const model::NodeSchema& owner, const model::PortSchema& port) {
    switch (port.domain) {
        case model::PortDomain::perVoice:
            return model::NodeScope::perVoice;
        case model::PortDomain::global:
            return model::NodeScope::global;
        case model::PortDomain::sameAsNode:
            return owner.scope;
    }
    std::terminate();
}

void addError(
    CompileResult& result,
    DiagnosticCode code,
    std::optional<model::NodeId> nodeId,
    std::optional<model::PortId> portId,
    std::string message) {
    result.diagnostics.push_back({code, Severity::error, std::move(nodeId), std::move(portId), std::move(message)});
}

const model::PortSchema* findPort(const model::NodeSchema& schema, const model::PortId& id) {
    const auto found = std::ranges::find(schema.ports, id, &model::PortSchema::id);
    return found == schema.ports.end() ? nullptr : &*found;
}

const model::ParameterSchema* findParameter(const model::NodeSchema& schema, const model::ParameterId& id) {
    const auto found = std::ranges::find(schema.parameters, id, &model::ParameterSchema::id);
    return found == schema.parameters.end() ? nullptr : &*found;
}

std::optional<ResolvedEndpoint> resolveEndpoint(
    const model::Endpoint& endpoint,
    const NodeRecords& nodes,
    const NodeSchemas& schemas,
    CompileResult& result) {
    const auto node = nodes.find(endpoint.nodeId);
    if (node == nodes.end()) {
        addError(result, DiagnosticCode::missingEndpointNode, endpoint.nodeId, endpoint.portId,
                 "A connection endpoint refers to a node that does not exist.");
        return std::nullopt;
    }

    if (node->second.size() != 1) return std::nullopt;

    const auto schema = schemas.find(endpoint.nodeId);
    if (schema == schemas.end() || schema->second == nullptr) return std::nullopt;

    const auto* port = findPort(*schema->second, endpoint.portId);
    if (port == nullptr) {
        addError(result, DiagnosticCode::missingPort, endpoint.nodeId, endpoint.portId,
                 "A connection endpoint refers to a port that is not in the node schema.");
        return std::nullopt;
    }

    return ResolvedEndpoint{*schema->second, *port};
}

void sortDiagnostics(std::vector<Diagnostic>& diagnostics) {
    std::ranges::sort(diagnostics, [](const Diagnostic& left, const Diagnostic& right) {
        if (left.code != right.code) return left.code < right.code;
        if (left.nodeId != right.nodeId) return left.nodeId < right.nodeId;
        if (left.portId != right.portId) return left.portId < right.portId;
        if (left.severity != right.severity) return left.severity < right.severity;
        return left.message < right.message;
    });
}
} // namespace

CompileResult GraphCompiler::compile(const model::GraphSnapshot& graph, const model::SchemaRegistry& registry) const {
    CompileResult result;
    if (graph.nodes.size() > limits_.maxNodes) {
        addError(
            result,
            DiagnosticCode::resourceLimitExceeded,
            std::nullopt,
            std::nullopt,
            "Graph node count " + std::to_string(graph.nodes.size()) + " exceeds the allowed maximum " +
                std::to_string(limits_.maxNodes) + ".");
        return result;
    }

    NodeRecords nodes;
    NodeSchemas schemas;
    detail::DependencyGraph perVoiceDependencies;
    detail::DependencyGraph globalDependencies;
    std::size_t perVoiceNodeCount = 0;
    std::size_t globalNodeCount = 0;

    for (const auto& node : graph.nodes) {
        nodes[node.id].push_back(&node);
    }

    for (const auto& [nodeId, records] : nodes) {
        if (records.size() > 1) {
            addError(result, DiagnosticCode::duplicateNodeId, nodeId, std::nullopt,
                     "More than one node uses the same stable node ID.");
        }

        for (const auto* node : records) {
            const auto* schema = registry.find(node->typeId);
            if (schema == nullptr) {
                addError(result, DiagnosticCode::missingNodeType, node->id, std::nullopt,
                         "No schema is registered for node type '" + node->typeId.value + "'.");
                continue;
            }

            if (node->schemaVersion != schema->schemaVersion) {
                addError(result, DiagnosticCode::unsupportedSchemaVersion, node->id, std::nullopt,
                         "The node's schema version is not supported by the registered schema.");
                continue;
            }

            for (const auto& [parameterId, value] : node->parameters) {
                const auto* parameter = findParameter(*schema, parameterId);
                if (parameter == nullptr) {
                    addError(result, DiagnosticCode::unknownParameter, node->id, std::nullopt,
                             "The node stores an unknown parameter '" + parameterId.value + "'.");
                } else if (!std::isfinite(value) ||
                           !(parameter->minimum <= value && value <= parameter->maximum)) {
                    addError(result, DiagnosticCode::parameterOutOfRange, node->id, std::nullopt,
                             "A stored parameter value is outside its inclusive schema range.");
                }
            }
        }

        if (records.size() == 1) {
            const auto* schema = registry.find(records.front()->typeId);
            if (schema != nullptr && records.front()->schemaVersion == schema->schemaVersion) {
                schemas.emplace(nodeId, schema);
            }
        }
    }

    for (const auto& [nodeId, schema] : schemas) {
        if (schema->scope == model::NodeScope::perVoice) {
            perVoiceDependencies.addNode(nodeId);
            ++perVoiceNodeCount;
        } else {
            globalDependencies.addNode(nodeId);
            ++globalNodeCount;
        }
    }

    std::vector<const model::Connection*> connections;
    connections.reserve(graph.connections.size());
    for (const auto& connection : graph.connections) connections.push_back(&connection);
    std::ranges::sort(connections, [](const auto* left, const auto* right) { return *left < *right; });

    std::set<model::Endpoint> connectedInputs;
    for (const auto* connection : connections) {
        const auto source = resolveEndpoint(connection->from, nodes, schemas, result);
        const auto target = resolveEndpoint(connection->to, nodes, schemas, result);

        if (!source || !target) continue;

        if (target->portSchema.direction == model::PortDirection::input &&
            !connectedInputs.insert(connection->to).second) {
            addError(result, DiagnosticCode::duplicateInputConnection, connection->to.nodeId, connection->to.portId,
                     "More than one connection writes to the same input port.");
        }

        if (source->portSchema.direction != model::PortDirection::output) {
            addError(result, DiagnosticCode::wrongPortDirection, connection->from.nodeId, connection->from.portId,
                     "The source endpoint is not an output port.");
        }
        if (target->portSchema.direction != model::PortDirection::input) {
            addError(result, DiagnosticCode::wrongPortDirection, connection->to.nodeId, connection->to.portId,
                     "The target endpoint is not an input port.");
        }
        if (source->portSchema.kind != target->portSchema.kind) {
            addError(result, DiagnosticCode::portKindMismatch, connection->to.nodeId, connection->to.portId,
                     "Connected ports have different kinds.");
        }
        if (source->portSchema.kind == model::PortKind::audio &&
            target->portSchema.kind == model::PortKind::audio &&
            source->portSchema.channels != target->portSchema.channels) {
            addError(result, DiagnosticCode::channelCountMismatch, connection->to.nodeId, connection->to.portId,
                     "Connected audio ports have different channel counts.");
        }

        const bool domainsMatch = resolveDomain(source->nodeSchema, source->portSchema) ==
                                  resolveDomain(target->nodeSchema, target->portSchema);
        if (!domainsMatch) {
            addError(result, DiagnosticCode::domainMismatch, connection->to.nodeId, connection->to.portId,
                     "Connected ports belong to different processing domains.");
        }

        const bool directionsMatch = source->portSchema.direction == model::PortDirection::output &&
                                     target->portSchema.direction == model::PortDirection::input;
        const bool sameSchedule = source->nodeSchema.scope == target->nodeSchema.scope;
        if (directionsMatch && domainsMatch && sameSchedule && !target->nodeSchema.breaksDependencyCycle) {
            auto& dependencies = source->nodeSchema.scope == model::NodeScope::perVoice
                                     ? perVoiceDependencies
                                     : globalDependencies;
            dependencies.addDependency(connection->from.nodeId, connection->to.nodeId);
        }
    }

    auto perVoiceOrder = perVoiceDependencies.topologicalOrder();
    auto globalOrder = globalDependencies.topologicalOrder();
    if (perVoiceOrder.size() != perVoiceNodeCount || globalOrder.size() != globalNodeCount) {
        addError(result, DiagnosticCode::cycleDetected, std::nullopt, std::nullopt,
                 "The graph contains a current-block dependency cycle.");
    }

    const bool validationFailed = std::ranges::any_of(result.diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.severity == Severity::error;
    });
    if (validationFailed) {
        sortDiagnostics(result.diagnostics);
        return result;
    }

    auto buffers = detail::planBuffers(graph, registry, perVoiceOrder, globalOrder);
    auto physicalChannels = std::uint64_t{};
    auto physicalChannelCountOverflow = buffers.physicalChannelCountOverflow;
    if (!physicalChannelCountOverflow && limits_.maxVoices != 0 &&
        buffers.perVoicePhysicalChannels >
            std::numeric_limits<std::uint64_t>::max() / limits_.maxVoices) {
        physicalChannelCountOverflow = true;
    } else if (!physicalChannelCountOverflow) {
        physicalChannels = buffers.perVoicePhysicalChannels * limits_.maxVoices;
        if (buffers.globalPhysicalChannels >
            std::numeric_limits<std::uint64_t>::max() - physicalChannels) {
            physicalChannelCountOverflow = true;
        } else {
            physicalChannels += buffers.globalPhysicalChannels;
        }
    }
    if (physicalChannelCountOverflow || physicalChannels > limits_.maxPhysicalBufferChannels) {
        const auto actual = physicalChannelCountOverflow
                                ? std::string{"overflow beyond 64-bit accounting"}
                                : std::to_string(physicalChannels);
        addError(
            result,
            DiagnosticCode::resourceLimitExceeded,
            std::nullopt,
            std::nullopt,
            "Physical buffer channel count " + actual +
                " exceeds the allowed maximum " + std::to_string(limits_.maxPhysicalBufferChannels) + ".");
        sortDiagnostics(result.diagnostics);
        return result;
    }

    result.graph.emplace();
    result.graph->voiceBudget = limits_.maxVoices;
    result.graph->perVoiceOrder = std::move(perVoiceOrder);
    result.graph->globalOrder = std::move(globalOrder);
    result.graph->audioBuffers = std::move(buffers.audio);
    result.graph->controlBuffers = std::move(buffers.control);

    for (const auto& [nodeId, records] : nodes) {
        const auto schema = schemas.find(nodeId);
        result.graph->nodes.push_back({*records.front(), schema->second->scope});
    }

    for (const auto* connection : connections) {
        const auto source = resolveEndpoint(connection->from, nodes, schemas, result);
        const auto kind = source->portSchema.kind;
        std::uint32_t slot = 0;
        const auto& assignments = kind == model::PortKind::audio
                                      ? result.graph->audioBuffers
                                      : result.graph->controlBuffers;
        if (kind == model::PortKind::audio || kind == model::PortKind::control) {
            const auto assignment =
                std::ranges::find(assignments, connection->from, &BufferAssignment::output);
            if (assignment != assignments.end()) slot = assignment->slot;
        }
        result.graph->connections.push_back({*connection, kind, slot});
    }
    return result;
}

CompileResult previewConnection(
    const model::GraphSnapshot& graph,
    const model::SchemaRegistry& registry,
    const model::Connection& connection,
    CompilerLimits limits) {
    auto copy = graph;
    copy.connections.push_back(connection);
    return GraphCompiler{limits}.compile(copy, registry);
}
} // namespace nodsynth::compiler
