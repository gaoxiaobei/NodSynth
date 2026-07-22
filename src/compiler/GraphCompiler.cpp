#include <nodsynth/compiler/GraphCompiler.h>

#include <algorithm>
#include <cmath>
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
    NodeRecords nodes;
    NodeSchemas schemas;
    std::set<model::NodeId> unsupportedVersions;

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
                unsupportedVersions.insert(node->id);
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

    std::vector<const model::Connection*> connections;
    connections.reserve(graph.connections.size());
    for (const auto& connection : graph.connections) connections.push_back(&connection);
    std::ranges::sort(connections, [](const auto* left, const auto* right) { return *left < *right; });

    std::set<model::Endpoint> connectedInputs;
    for (const auto* connection : connections) {
        if (unsupportedVersions.contains(connection->from.nodeId) ||
            unsupportedVersions.contains(connection->to.nodeId)) {
            continue;
        }

        const auto source = resolveEndpoint(connection->from, nodes, schemas, result);
        const auto target = resolveEndpoint(connection->to, nodes, schemas, result);

        if (target && target->portSchema.direction == model::PortDirection::input &&
            !connectedInputs.insert(connection->to).second) {
            addError(result, DiagnosticCode::duplicateInputConnection, connection->to.nodeId, connection->to.portId,
                     "More than one connection writes to the same input port.");
        }

        if (!source || !target) continue;

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
    }

    sortDiagnostics(result.diagnostics);
    const bool hasError = std::ranges::any_of(result.diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.severity == Severity::error;
    });
    if (!hasError) result.graph.emplace();
    return result;
}
} // namespace nodsynth::compiler
