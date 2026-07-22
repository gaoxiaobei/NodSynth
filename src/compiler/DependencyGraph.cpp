#include "DependencyGraph.h"

#include <cstddef>
#include <map>
#include <set>
#include <utility>

namespace nodsynth::compiler::detail {
void DependencyGraph::addNode(model::NodeId id) {
    outgoing_.try_emplace(std::move(id));
}

void DependencyGraph::addDependency(model::NodeId before, model::NodeId after) {
    addNode(before);
    addNode(after);
    outgoing_.at(before).insert(std::move(after));
}

std::vector<model::NodeId> DependencyGraph::topologicalOrder() const {
    std::map<model::NodeId, std::size_t> incomingCounts;
    for (const auto& [nodeId, outgoing] : outgoing_) {
        incomingCounts.try_emplace(nodeId, 0);
        for (const auto& dependent : outgoing) ++incomingCounts[dependent];
    }

    std::set<model::NodeId> ready;
    for (const auto& [nodeId, incomingCount] : incomingCounts) {
        if (incomingCount == 0) ready.insert(nodeId);
    }

    std::vector<model::NodeId> order;
    order.reserve(outgoing_.size());
    while (!ready.empty()) {
        const auto nodeId = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(nodeId);

        for (const auto& dependent : outgoing_.at(nodeId)) {
            auto& incomingCount = incomingCounts.at(dependent);
            --incomingCount;
            if (incomingCount == 0) ready.insert(dependent);
        }
    }

    if (order.size() != outgoing_.size()) order.clear();
    return order;
}
} // namespace nodsynth::compiler::detail
