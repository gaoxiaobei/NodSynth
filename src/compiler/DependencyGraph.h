#pragma once

#include <map>
#include <set>
#include <vector>

#include <nodsynth/model/Identifiers.h>

namespace nodsynth::compiler::detail {
class DependencyGraph {
public:
    void addNode(model::NodeId id);
    void addDependency(model::NodeId before, model::NodeId after);
    [[nodiscard]] std::vector<model::NodeId> topologicalOrder() const;

private:
    std::map<model::NodeId, std::set<model::NodeId>> outgoing_;
};
} // namespace nodsynth::compiler::detail
