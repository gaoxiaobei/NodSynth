#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nodsynth/model/Identifiers.h>

namespace nodsynth::model {
struct Point {
    float x{};
    float y{};
};

struct Endpoint {
    NodeId nodeId;
    PortId portId;
    auto operator<=>(const Endpoint&) const = default;
};

struct Connection {
    Endpoint from;
    Endpoint to;
    auto operator<=>(const Connection&) const = default;
};

struct NodeRecord {
    NodeId id;
    NodeTypeId typeId;
    std::uint32_t schemaVersion;
    std::map<ParameterId, double> parameters;
    std::string opaqueStateJson;
    Point position;
};

struct GraphSnapshot {
    std::vector<NodeRecord> nodes;
    std::vector<Connection> connections;
};

class GraphDocument {
public:
    [[nodiscard]] const std::vector<NodeRecord>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::vector<Connection>& connections() const noexcept { return connections_; }
    [[nodiscard]] GraphSnapshot snapshot() const { return {nodes_, connections_}; }
    [[nodiscard]] const NodeRecord* findNode(const NodeId& id) const noexcept;

private:
    friend class GraphEditor;

    std::vector<NodeRecord> nodes_;
    std::vector<Connection> connections_;
};
} // namespace nodsynth::model
