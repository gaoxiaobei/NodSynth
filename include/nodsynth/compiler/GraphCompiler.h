#pragma once

#include <cstdint>

#include <nodsynth/compiler/CompiledGraph.h>
#include <nodsynth/model/SchemaRegistry.h>

namespace nodsynth::compiler {
struct CompilerLimits {
    std::uint32_t maxNodes{4096};
    std::uint32_t maxPhysicalBufferChannels{8192};
    std::uint32_t maxVoices{16};
};

class GraphCompiler {
public:
    explicit GraphCompiler(CompilerLimits limits = {}) : limits_(limits) {}

    [[nodiscard]] CompileResult compile(
        const model::GraphSnapshot& graph,
        const model::SchemaRegistry& registry) const;

private:
    CompilerLimits limits_;
};

[[nodiscard]] CompileResult previewConnection(
    const model::GraphSnapshot& graph,
    const model::SchemaRegistry& registry,
    const model::Connection& connection,
    CompilerLimits limits = {});
} // namespace nodsynth::compiler
