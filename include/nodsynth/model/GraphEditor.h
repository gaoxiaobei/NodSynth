#pragma once

#include <functional>
#include <vector>

#include <nodsynth/model/GraphDocument.h>

namespace nodsynth::model {
enum class ChangeKind { none, structure, parameter, layout };

class GraphEditor {
public:
    [[nodiscard]] const GraphDocument& document() const noexcept { return document_; }
    [[nodiscard]] std::uint64_t structureRevision() const noexcept { return structureRevision_; }
    [[nodiscard]] ChangeKind recentChange() const noexcept { return recentChange_; }

    bool addNode(NodeRecord node);
    bool removeNode(const NodeId& id);
    bool connect(Connection connection);
    bool disconnect(Connection connection);
    bool setParameter(const NodeId& id, const ParameterId& parameterId, double value);
    bool moveNode(const NodeId& id, Point position);
    bool setViewport(Viewport viewport);
    void load(GraphSnapshot snapshot);
    bool undo();
    bool redo();

private:
    struct Edit {
        std::function<void(GraphDocument&)> apply;
        std::function<void(GraphDocument&)> revert;
        ChangeKind kind{ChangeKind::structure};
    };

    void commit(Edit edit);

    GraphDocument document_;
    std::vector<Edit> undo_;
    std::vector<Edit> redo_;
    std::uint64_t structureRevision_{0};
    ChangeKind recentChange_{ChangeKind::none};
};
} // namespace nodsynth::model
