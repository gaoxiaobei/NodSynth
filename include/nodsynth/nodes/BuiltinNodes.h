#pragma once

#include <nodsynth/model/GraphDocument.h>
#include <nodsynth/model/SchemaRegistry.h>
#include <nodsynth/runtime/DspNode.h>

namespace nodsynth::nodes {
[[nodiscard]] model::SchemaRegistry builtinRegistry();
[[nodiscard]] const runtime::ImplementationRegistry& builtinImplementations();
[[nodiscard]] model::GraphSnapshot sinePatch();
[[nodiscard]] model::GraphSnapshot filterPatch();
[[nodiscard]] model::GraphSnapshot delayPatch();
[[nodiscard]] model::GraphSnapshot stereoFilterPatch();
[[nodiscard]] model::GraphSnapshot unisonPatch(bool pad = false);
} // namespace nodsynth::nodes
