#include <algorithm>

#include <nodsynth/model/GraphDocument.h>

namespace nodsynth::model {
const NodeRecord* GraphDocument::findNode(const NodeId& id) const noexcept {
    const auto found = std::ranges::find(nodes_, id, &NodeRecord::id);
    return found == nodes_.end() ? nullptr : &*found;
}
} // namespace nodsynth::model
