#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

#include <nodsynth/model/GraphEditor.h>

namespace nodsynth::model {
bool GraphEditor::addNode(NodeRecord node) {
    if (document_.findNode(node.id) != nullptr) {
        return false;
    }

    commit({
        [node](GraphDocument& document) { document.nodes_.push_back(node); },
        [id = node.id](GraphDocument& document) {
            const auto found = std::ranges::find(document.nodes_, id, &NodeRecord::id);
            document.nodes_.erase(found);
        },
        ChangeKind::structure,
    });
    return true;
}

bool GraphEditor::removeNode(const NodeId& id) {
    const auto node = std::ranges::find(document_.nodes_, id, &NodeRecord::id);
    if (node == document_.nodes_.end()) {
        return false;
    }

    const NodeRecord removedNode = *node;
    const std::size_t nodeIndex = static_cast<std::size_t>(node - document_.nodes_.begin());
    std::vector<std::pair<std::size_t, Connection>> removedConnections;
    for (std::size_t index = 0; index < document_.connections_.size(); ++index) {
        const Connection& connection = document_.connections_[index];
        if (connection.from.nodeId == id || connection.to.nodeId == id) {
            removedConnections.emplace_back(index, connection);
        }
    }

    commit({
        [id](GraphDocument& document) {
            const auto node = std::ranges::find(document.nodes_, id, &NodeRecord::id);
            document.nodes_.erase(node);
            std::erase_if(document.connections_, [&id](const Connection& connection) {
                return connection.from.nodeId == id || connection.to.nodeId == id;
            });
        },
        [removedNode, nodeIndex, removedConnections](GraphDocument& document) {
            document.nodes_.insert(document.nodes_.begin() + static_cast<std::ptrdiff_t>(nodeIndex), removedNode);
            for (const auto& [index, connection] : removedConnections) {
                document.connections_.insert(
                    document.connections_.begin() + static_cast<std::ptrdiff_t>(index), connection);
            }
        },
        ChangeKind::structure,
    });
    return true;
}

bool GraphEditor::connect(Connection connection) {
    const auto duplicate = std::ranges::find(document_.connections_, connection);
    const auto inputOccupied = std::ranges::find(document_.connections_, connection.to, &Connection::to);
    if (duplicate != document_.connections_.end() || inputOccupied != document_.connections_.end()) {
        return false;
    }

    commit({
        [connection](GraphDocument& document) { document.connections_.push_back(connection); },
        [connection](GraphDocument& document) {
            const auto found = std::ranges::find(document.connections_, connection);
            document.connections_.erase(found);
        },
        ChangeKind::structure,
    });
    return true;
}

bool GraphEditor::disconnect(Connection connection) {
    const auto found = std::ranges::find(document_.connections_, connection);
    if (found == document_.connections_.end()) {
        return false;
    }

    const std::size_t index = static_cast<std::size_t>(found - document_.connections_.begin());
    commit({
        [connection](GraphDocument& document) {
            const auto found = std::ranges::find(document.connections_, connection);
            document.connections_.erase(found);
        },
        [connection, index](GraphDocument& document) {
            document.connections_.insert(
                document.connections_.begin() + static_cast<std::ptrdiff_t>(index), connection);
        },
        ChangeKind::structure,
    });
    return true;
}

bool GraphEditor::setParameter(const NodeId& id, const ParameterId& parameterId, double value) {
    const auto node = std::ranges::find(document_.nodes_, id, &NodeRecord::id);
    if (node == document_.nodes_.end()) {
        return false;
    }

    const auto previous = node->parameters.find(parameterId);
    const std::optional<double> previousValue =
        previous == node->parameters.end() ? std::nullopt : std::optional<double>{previous->second};
    commit({
        [id, parameterId, value](GraphDocument& document) {
            const auto node = std::ranges::find(document.nodes_, id, &NodeRecord::id);
            node->parameters[parameterId] = value;
        },
        [id, parameterId, previousValue](GraphDocument& document) {
            const auto node = std::ranges::find(document.nodes_, id, &NodeRecord::id);
            if (previousValue.has_value()) {
                node->parameters[parameterId] = *previousValue;
            } else {
                node->parameters.erase(parameterId);
            }
        },
        ChangeKind::parameter,
    });
    return true;
}

bool GraphEditor::moveNode(const NodeId& id, Point position) {
    const auto node = std::ranges::find(document_.nodes_, id, &NodeRecord::id);
    if (node == document_.nodes_.end() || !std::isfinite(position.x) || !std::isfinite(position.y)) return false;

    const Point previous = node->position;
    commit({
        [id, position](GraphDocument& document) {
            const auto node = std::ranges::find(document.nodes_, id, &NodeRecord::id);
            node->position = position;
        },
        [id, previous](GraphDocument& document) {
            const auto node = std::ranges::find(document.nodes_, id, &NodeRecord::id);
            node->position = previous;
        },
        ChangeKind::layout,
    });
    return true;
}

bool GraphEditor::setViewport(Viewport viewport) {
    if (!std::isfinite(viewport.originX) || !std::isfinite(viewport.originY) || !std::isfinite(viewport.zoom) ||
        viewport.zoom < 0.2f || viewport.zoom > 4.f) {
        return false;
    }
    document_.viewport_ = viewport;
    recentChange_ = ChangeKind::layout;
    return true;
}

void GraphEditor::load(GraphSnapshot snapshot) {
    document_.nodes_ = std::move(snapshot.nodes);
    document_.connections_ = std::move(snapshot.connections);
    undo_.clear();
    redo_.clear();
    ++structureRevision_;
    recentChange_ = ChangeKind::structure;
}

bool GraphEditor::undo() {
    if (undo_.empty()) {
        return false;
    }

    Edit edit = std::move(undo_.back());
    undo_.pop_back();
    edit.revert(document_);
    recentChange_ = edit.kind;
    if (edit.kind == ChangeKind::structure) ++structureRevision_;
    redo_.push_back(std::move(edit));
    return true;
}

bool GraphEditor::redo() {
    if (redo_.empty()) {
        return false;
    }

    Edit edit = std::move(redo_.back());
    redo_.pop_back();
    edit.apply(document_);
    recentChange_ = edit.kind;
    if (edit.kind == ChangeKind::structure) ++structureRevision_;
    undo_.push_back(std::move(edit));
    return true;
}

void GraphEditor::commit(Edit edit) {
    edit.apply(document_);
    recentChange_ = edit.kind;
    if (edit.kind == ChangeKind::structure) ++structureRevision_;
    redo_.clear();
    undo_.push_back(std::move(edit));
}
} // namespace nodsynth::model
