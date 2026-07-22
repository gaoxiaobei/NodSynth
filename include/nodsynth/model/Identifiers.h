#pragma once

#include <compare>
#include <string>

namespace nodsynth::model {
template <typename Tag>
struct StableId {
    std::string value;
    auto operator<=>(const StableId&) const = default;
};

struct NodeIdTag;
struct NodeTypeIdTag;
struct PortIdTag;
struct ParameterIdTag;
using NodeId = StableId<NodeIdTag>;
using NodeTypeId = StableId<NodeTypeIdTag>;
using PortId = StableId<PortIdTag>;
using ParameterId = StableId<ParameterIdTag>;
} // namespace nodsynth::model
