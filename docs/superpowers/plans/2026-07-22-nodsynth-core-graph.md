# NodSynth Core Graph Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the platform-independent node schema, editable graph document, undo/redo command layer, graph validator/compiler, and a runnable `nod_graphcheck` smoke tool.

**Architecture:** `nod_model` owns serializable graph concepts without UI or DSP dependencies. `nod_compiler` consumes immutable graph snapshots plus a schema registry and emits structured diagnostics and a deterministic compiled graph; the command-line tool proves these modules can be embedded without JUCE.

**Tech Stack:** C++20, CMake 3.27+, Ninja, Catch2 3.15.0, standard library only in production targets.

## Global Constraints

- The first release target is a Linux standalone application, while Windows and macOS must continue to compile.
- Stable IDs, never display names, define node types, ports, parameters, instances, and saved connections.
- Public model/compiler headers must not include JUCE or platform headers.
- Port kinds are exactly `Audio`, `Control`, `Gate`, and `Note`; incompatible kinds never connect implicitly.
- Ordinary connections form a DAG. Only a node schema marked as a latency break can interrupt a dependency cycle.
- `PerVoice` and `Global` domains cannot cross except through an explicitly declared voice-boundary port.
- Graph errors carry code, severity, node ID, port ID, and a user-readable message.
- The compiler output must be deterministic for the same graph, independent of insertion order.
- Production code uses C++20; dependency versions are pinned, not floating branches.
- JUCE is not introduced in this phase. Before public distribution, choose an AGPLv3-compatible project licence or obtain a commercial JUCE licence; do not add a repository licence by assumption.

## Scope Boundary

This is the first of four implementation plans derived from the approved MVP design:

1. **This plan:** core graph model and compiler, ending in a runnable graph-checking tool.
2. Realtime DSP runtime, voice allocator, built-in nodes, plan exchange, and offline renderer.
3. JSON persistence, atomic saves, migrations, Missing Node recovery, and the stable C extension boundary.
4. JUCE audio/MIDI shell, node canvas and inspector, live compilation, presets, CI hardening, and end-to-end acceptance.

The later phases depend on the interfaces defined here, but no later-phase feature is partially implemented in this plan.

## Phase 1 Spec Traceability

| Approved design requirement | Implemented by |
|---|---|
| Stable type/instance/port/parameter IDs and node schema | Tasks 2–3 |
| Audio, Control, Gate, and Note port types | Tasks 2, 4, and 7 |
| PerVoice/Global scope and explicit Voice Mix boundary | Task 5 |
| DAG enforcement and explicit latency break | Task 5 |
| Structured diagnostics with node/port locations | Task 4 |
| Deterministic topology and precomputed buffer plan | Tasks 5–6 |
| Resource-budget rejection | Task 6 |
| Platform-independent core and three-platform compile checks | Tasks 1 and 7 |
| Unit tests for compatibility, cycles, scope, ordering, and limits | Tasks 2–7 |

## Planned File Structure

```text
CMakeLists.txt                         Project options and top-level targets
CMakePresets.json                     Reproducible developer configure/build/test commands
cmake/Dependencies.cmake              Pinned test-only dependency declarations
include/nodsynth/model/Identifiers.h  Strong stable-ID wrappers
include/nodsynth/model/NodeSchema.h   Port, parameter, scope, and node schemas
include/nodsynth/model/SchemaRegistry.h  Schema lookup and registration
include/nodsynth/model/GraphDocument.h   Nodes, connections, and graph snapshots
include/nodsynth/model/GraphEditor.h     Reversible graph-edit commands
src/model/*.cpp                       Model implementations
include/nodsynth/compiler/Diagnostic.h  Structured compiler diagnostics
include/nodsynth/compiler/CompiledGraph.h Compiler output and buffer assignments
include/nodsynth/compiler/GraphCompiler.h Public compiler entry point
src/compiler/*.cpp                    Validation, dependency analysis, and planning
apps/graphcheck/Main.cpp              Runnable valid/invalid graph smoke scenarios
tests/model/*.cpp                     Model and command tests
tests/compiler/*.cpp                  Compiler tests
.github/workflows/ci.yml              Linux, Windows, and macOS core checks
```

---

### Task 1: Reproducible C++ Build and Test Harness

**Files:**
- Modify: `.gitignore`
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `cmake/Dependencies.cmake`
- Create: `include/nodsynth/model/BuildInfo.h`
- Create: `src/model/BuildInfo.cpp`
- Create: `tests/CMakeLists.txt`
- Create: `tests/model/BuildInfoTests.cpp`

**Interfaces:**
- Consumes: No project code.
- Produces: `nodsynth::model::buildInfo()` returning `BuildInfo`; CMake targets `nod_model` and `nod_tests` used by later tasks.

- [ ] **Step 1: Write the failing build-info test**

```cpp
// tests/model/BuildInfoTests.cpp
#include <catch2/catch_test_macros.hpp>
#include <nodsynth/model/BuildInfo.h>

TEST_CASE("build info exposes the core API version") {
    const auto info = nodsynth::model::buildInfo();
    REQUIRE(info.coreApiMajor == 1);
    REQUIRE(info.coreApiMinor == 0);
    REQUIRE(info.projectName == "NodSynth");
}
```

- [ ] **Step 2: Add the CMake skeleton and verify the test target fails**

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(NodSynth VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NODSYNTH_BUILD_TESTS "Build NodSynth tests" ON)
option(NODSYNTH_BUILD_GRAPHCHECK "Build the graph-checking CLI" ON)

add_library(nod_model src/model/BuildInfo.cpp)
target_include_directories(nod_model PUBLIC include)
target_compile_features(nod_model PUBLIC cxx_std_20)

if(NODSYNTH_BUILD_TESTS)
  include(CTest)
  include(cmake/Dependencies.cmake)
  add_subdirectory(tests)
endif()
```

```cmake
# cmake/Dependencies.cmake
include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.15.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)
```

```cmake
# tests/CMakeLists.txt
add_executable(nod_tests model/BuildInfoTests.cpp)
target_link_libraries(nod_tests PRIVATE nod_model Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(nod_tests)
```

Run: `cmake --preset dev && cmake --build --preset dev --target nod_tests -j2`

Expected: compilation fails because `nodsynth/model/BuildInfo.h` does not exist.

- [ ] **Step 3: Add presets and the minimal BuildInfo implementation**

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "dev",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/dev",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "NODSYNTH_BUILD_TESTS": "ON"
      }
    }
  ],
  "buildPresets": [{ "name": "dev", "configurePreset": "dev" }],
  "testPresets": [{ "name": "dev", "configurePreset": "dev", "output": { "outputOnFailure": true } }]
}
```

```cpp
// include/nodsynth/model/BuildInfo.h
#pragma once
#include <string_view>

namespace nodsynth::model {
struct BuildInfo {
    int coreApiMajor;
    int coreApiMinor;
    std::string_view projectName;
};

[[nodiscard]] BuildInfo buildInfo() noexcept;
} // namespace nodsynth::model
```

```cpp
// src/model/BuildInfo.cpp
#include <nodsynth/model/BuildInfo.h>

namespace nodsynth::model {
BuildInfo buildInfo() noexcept { return {1, 0, "NodSynth"}; }
} // namespace nodsynth::model
```

Append these entries to `.gitignore`:

```gitignore
/build/
/compile_commands.json
```

- [ ] **Step 4: Run the test suite**

Run: `cmake --preset dev && cmake --build --preset dev --target nod_tests -j2 && ctest --preset dev`

Expected: one discovered test passes.

- [ ] **Step 5: Commit the build foundation**

```bash
git add .gitignore CMakeLists.txt CMakePresets.json cmake/Dependencies.cmake include/nodsynth/model/BuildInfo.h src/model/BuildInfo.cpp tests
git commit -m "build: add core CMake test harness"
```

---

### Task 2: Stable IDs, Node Schemas, and Schema Registry

**Files:**
- Create: `include/nodsynth/model/Identifiers.h`
- Create: `include/nodsynth/model/NodeSchema.h`
- Create: `include/nodsynth/model/SchemaRegistry.h`
- Create: `src/model/SchemaRegistry.cpp`
- Create: `tests/model/SchemaRegistryTests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `nod_model` target from Task 1.
- Produces: `NodeId`, `NodeTypeId`, `PortId`, `ParameterId`, `NodeSchema`, and `SchemaRegistry::registerSchema/find`; the graph model and compiler use these exact types.

- [ ] **Step 1: Write registry tests for stable lookup and duplicate rejection**

```cpp
// tests/model/SchemaRegistryTests.cpp
#include <catch2/catch_test_macros.hpp>
#include <nodsynth/model/SchemaRegistry.h>

using namespace nodsynth::model;

static NodeSchema oscillatorSchema() {
    return {
        .typeId = NodeTypeId{"org.nodsynth.oscillator.analog"},
        .schemaVersion = 1,
        .displayName = "Oscillator",
        .category = "Sources/Oscillators",
        .sourceId = "org.nodsynth.builtin",
        .scope = NodeScope::perVoice,
        .ports = {
            {PortId{"frequency"}, "Frequency", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode},
            {PortId{"audio"}, "Audio", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

TEST_CASE("schema registry finds a registered stable type ID") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(oscillatorSchema()));
    REQUIRE(registry.find(NodeTypeId{"org.nodsynth.oscillator.analog"}) != nullptr);
}

TEST_CASE("schema registry rejects duplicate type IDs") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(oscillatorSchema()));
    REQUIRE_FALSE(registry.registerSchema(oscillatorSchema()));
    REQUIRE(registry.size() == 1);
}
```

- [ ] **Step 2: Run the focused tests and confirm missing-type failures**

Run: `cmake --build --preset dev --target nod_tests -j2`

Expected: compilation fails because `SchemaRegistry.h` and the ID/schema types do not exist.

- [ ] **Step 3: Implement strongly typed IDs and node schemas**

```cpp
// include/nodsynth/model/Identifiers.h
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
```

```cpp
// include/nodsynth/model/NodeSchema.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <nodsynth/model/Identifiers.h>

namespace nodsynth::model {
enum class PortDirection { input, output };
enum class PortKind { audio, control, gate, note };
enum class NodeScope { perVoice, global };
enum class PortDomain { sameAsNode, perVoice, global };
enum class ParameterScale { linear, logarithmic };

struct PortSchema {
    PortId id;
    std::string displayName;
    PortDirection direction;
    PortKind kind;
    std::uint32_t channels;
    PortDomain domain;
};

struct ParameterSchema {
    ParameterId id;
    std::string displayName;
    std::string unit;
    double minimum;
    double maximum;
    double defaultValue;
    ParameterScale scale;
    bool modulatable;
};

struct NodeSchema {
    NodeTypeId typeId;
    std::uint32_t schemaVersion;
    std::string displayName;
    std::string category;
    std::string sourceId;
    NodeScope scope;
    std::vector<PortSchema> ports;
    std::vector<ParameterSchema> parameters;
    bool breaksDependencyCycle;
};
} // namespace nodsynth::model
```

- [ ] **Step 4: Implement registry validation**

```cpp
// include/nodsynth/model/SchemaRegistry.h
#pragma once
#include <map>
#include <nodsynth/model/NodeSchema.h>

namespace nodsynth::model {
class SchemaRegistry {
public:
    bool registerSchema(NodeSchema schema);
    [[nodiscard]] const NodeSchema* find(const NodeTypeId& typeId) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return schemas_.size(); }
private:
    std::map<NodeTypeId, NodeSchema> schemas_;
};
} // namespace nodsynth::model
```

```cpp
// src/model/SchemaRegistry.cpp
#include <nodsynth/model/SchemaRegistry.h>
#include <set>

namespace nodsynth::model {
bool SchemaRegistry::registerSchema(NodeSchema schema) {
    if (schema.typeId.value.empty() || schema.schemaVersion == 0 || schema.sourceId.empty()) return false;
    std::set<PortId> portIds;
    for (const auto& port : schema.ports) {
        if (port.id.value.empty() || port.channels == 0 || !portIds.insert(port.id).second) return false;
    }
    std::set<ParameterId> parameterIds;
    for (const auto& parameter : schema.parameters) {
        if (parameter.id.value.empty() || parameter.minimum > parameter.defaultValue ||
            parameter.defaultValue > parameter.maximum || !parameterIds.insert(parameter.id).second) return false;
        if (parameter.scale == ParameterScale::logarithmic && parameter.minimum <= 0.0) return false;
    }
    return schemas_.emplace(schema.typeId, std::move(schema)).second;
}

const NodeSchema* SchemaRegistry::find(const NodeTypeId& typeId) const noexcept {
    const auto found = schemas_.find(typeId);
    return found == schemas_.end() ? nullptr : &found->second;
}
} // namespace nodsynth::model
```

Add the sources to their targets:

```cmake
target_sources(nod_model PRIVATE src/model/SchemaRegistry.cpp)
target_sources(nod_tests PRIVATE model/SchemaRegistryTests.cpp)
```

- [ ] **Step 5: Run model tests**

Run: `cmake --build --preset dev --target nod_tests -j2 && ctest --preset dev -R "schema registry|build info"`

Expected: all focused tests pass.

- [ ] **Step 6: Commit the schema contract**

```bash
git add CMakeLists.txt include/nodsynth/model src/model tests
git commit -m "feat: add stable node schema registry"
```

---

### Task 3: Editable Graph Document and Undo/Redo Commands

**Files:**
- Create: `include/nodsynth/model/GraphDocument.h`
- Create: `include/nodsynth/model/GraphEditor.h`
- Create: `src/model/GraphDocument.cpp`
- Create: `src/model/GraphEditor.cpp`
- Create: `tests/model/GraphEditorTests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: ID and schema types from Task 2.
- Produces: `GraphDocument::snapshot`, `GraphEditor::addNode/removeNode/connect/disconnect/setParameter/undo/redo`; the compiler consumes `GraphSnapshot` only, never mutable editor state.

- [ ] **Step 1: Write tests for graph mutation and reversible connection removal**

```cpp
// tests/model/GraphEditorTests.cpp
#include <catch2/catch_test_macros.hpp>
#include <nodsynth/model/GraphEditor.h>

using namespace nodsynth::model;

TEST_CASE("remove node undo restores the node and its connections") {
    GraphEditor editor;
    editor.addNode({NodeId{"osc-1"}, NodeTypeId{"osc"}, 1, {{ParameterId{"gain"}, 0.5}}, "{}", {10.0F, 20.0F}});
    editor.addNode({NodeId{"out-1"}, NodeTypeId{"out"}, 1, {}, "{}", {300.0F, 20.0F}});
    editor.connect({{NodeId{"osc-1"}, PortId{"audio"}}, {NodeId{"out-1"}, PortId{"audio"}}});

    REQUIRE(editor.removeNode(NodeId{"osc-1"}));
    REQUIRE(editor.document().nodes().size() == 1);
    REQUIRE(editor.document().connections().empty());

    REQUIRE(editor.undo());
    REQUIRE(editor.document().nodes().size() == 2);
    REQUIRE(editor.document().connections().size() == 1);
    REQUIRE(editor.redo());
    REQUIRE(editor.document().nodes().size() == 1);
}

TEST_CASE("an input accepts at most one connection") {
    GraphEditor editor;
    editor.addNode({NodeId{"a"}, NodeTypeId{"source"}, 1, {}, "{}", {}});
    editor.addNode({NodeId{"b"}, NodeTypeId{"source"}, 1, {}, "{}", {}});
    editor.addNode({NodeId{"c"}, NodeTypeId{"sink"}, 1, {}, "{}", {}});
    REQUIRE(editor.connect({{NodeId{"a"}, PortId{"out"}}, {NodeId{"c"}, PortId{"in"}}}));
    REQUIRE_FALSE(editor.connect({{NodeId{"b"}, PortId{"out"}}, {NodeId{"c"}, PortId{"in"}}}));
}
```

- [ ] **Step 2: Run the focused test and verify it fails to compile**

Run: `cmake --build --preset dev --target nod_tests -j2`

Expected: compilation fails because `GraphEditor.h` does not exist.

- [ ] **Step 3: Implement the immutable snapshot data model**

```cpp
// include/nodsynth/model/GraphDocument.h
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nodsynth/model/Identifiers.h>

namespace nodsynth::model {
struct Point { float x{}; float y{}; };
struct Endpoint { NodeId nodeId; PortId portId; auto operator<=>(const Endpoint&) const = default; };
struct Connection { Endpoint from; Endpoint to; auto operator<=>(const Connection&) const = default; };
struct NodeRecord {
    NodeId id;
    NodeTypeId typeId;
    std::uint32_t schemaVersion;
    std::map<ParameterId, double> parameters;
    std::string opaqueStateJson;
    Point position;
};
struct GraphSnapshot { std::vector<NodeRecord> nodes; std::vector<Connection> connections; };

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
```

Implement `findNode` with `std::ranges::find` in `src/model/GraphDocument.cpp`.

- [ ] **Step 4: Implement command-based editing**

```cpp
// include/nodsynth/model/GraphEditor.h
#pragma once
#include <functional>
#include <vector>
#include <nodsynth/model/GraphDocument.h>

namespace nodsynth::model {
class GraphEditor {
public:
    [[nodiscard]] const GraphDocument& document() const noexcept { return document_; }
    bool addNode(NodeRecord node);
    bool removeNode(const NodeId& id);
    bool connect(Connection connection);
    bool disconnect(Connection connection);
    bool setParameter(const NodeId& id, const ParameterId& parameterId, double value);
    bool undo();
    bool redo();
private:
    struct Edit { std::function<void(GraphDocument&)> apply; std::function<void(GraphDocument&)> revert; };
    void commit(Edit edit);
    GraphDocument document_;
    std::vector<Edit> undo_;
    std::vector<Edit> redo_;
};
} // namespace nodsynth::model
```

In `GraphEditor.cpp`, each public mutation must capture complete before/after values. `removeNode` captures the removed `NodeRecord`, its original vector index, and every incident `Connection`; `undo` restores all three exactly. `connect` rejects duplicate connections and a second connection targeting the same input endpoint. `commit` applies the edit, clears `redo_`, and pushes it to `undo_`.

- [ ] **Step 5: Add parameter and duplicate-node tests, then run all model tests**

```cpp
TEST_CASE("node IDs are unique and parameter edits undo") {
    GraphEditor editor;
    const NodeRecord node{NodeId{"node"}, NodeTypeId{"gain"}, 1, {{ParameterId{"gain"}, 0.5}}, "{}", {}};
    REQUIRE(editor.addNode(node));
    REQUIRE_FALSE(editor.addNode(node));
    REQUIRE(editor.setParameter(NodeId{"node"}, ParameterId{"gain"}, 0.8));
    REQUIRE(editor.document().nodes().front().parameters.at(ParameterId{"gain"}) == 0.8);
    REQUIRE(editor.undo());
    REQUIRE(editor.document().nodes().front().parameters.at(ParameterId{"gain"}) == 0.5);
}
```

Run: `cmake --build --preset dev --target nod_tests -j2 && ctest --preset dev -R "Graph|node IDs"`

Expected: all graph editor tests pass.

- [ ] **Step 6: Commit the graph document**

```bash
git add CMakeLists.txt include/nodsynth/model src/model tests
git commit -m "feat: add editable graph document"
```

---

### Task 4: Structured Graph Validation

**Files:**
- Create: `include/nodsynth/compiler/Diagnostic.h`
- Create: `include/nodsynth/compiler/CompiledGraph.h`
- Create: `include/nodsynth/compiler/GraphCompiler.h`
- Create: `src/compiler/GraphCompiler.cpp`
- Create: `tests/compiler/GraphValidationTests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `GraphSnapshot`, `NodeSchema`, and `SchemaRegistry` from Tasks 2–3.
- Produces: `GraphCompiler::compile(const GraphSnapshot&, const SchemaRegistry&) -> CompileResult`; later runtime plans consume `CompiledGraph` and its deterministic node orders/buffer slots.

- [ ] **Step 1: Write validation tests for missing schemas and incompatible ports**

```cpp
// tests/compiler/GraphValidationTests.cpp
#include <catch2/catch_test_macros.hpp>
#include <nodsynth/compiler/GraphCompiler.h>
#include "TestGraphBuilders.h"

using namespace nodsynth::model;
using namespace nodsynth::compiler;

TEST_CASE("compiler locates a missing node schema") {
    SchemaRegistry registry;
    GraphSnapshot graph{{{NodeId{"missing-1"}, NodeTypeId{"third.party.absent"}, 1, {}, "{}", {}}}, {}};
    const auto result = GraphCompiler{}.compile(graph, registry);
    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(result.diagnostics.front().code == DiagnosticCode::missingNodeType);
    REQUIRE(result.diagnostics.front().nodeId == NodeId{"missing-1"});
}

TEST_CASE("compiler rejects audio to control") {
    auto [registry, graph] = testGraphWithAudioSourceAndControlSink();
    const auto result = GraphCompiler{}.compile(graph, registry);
    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(result.diagnostics.front().code == DiagnosticCode::portKindMismatch);
    REQUIRE(result.diagnostics.front().portId == PortId{"frequency"});
}
```

Place shared schema builders in `tests/compiler/TestGraphBuilders.h`; implement `testGraphWithAudioSourceAndControlSink()` there with literal schemas and one incompatible connection.

- [ ] **Step 2: Run the compiler tests and verify missing headers fail**

Run: `cmake --build --preset dev --target nod_tests -j2`

Expected: compilation fails because `GraphCompiler.h` does not exist.

- [ ] **Step 3: Define diagnostics and compiler output**

```cpp
// include/nodsynth/compiler/Diagnostic.h
#pragma once
#include <optional>
#include <string>
#include <nodsynth/model/Identifiers.h>

namespace nodsynth::compiler {
enum class Severity { warning, error };
enum class DiagnosticCode {
    duplicateNodeId, missingNodeType, unsupportedSchemaVersion,
    missingEndpointNode, missingPort, wrongPortDirection,
    portKindMismatch, channelCountMismatch, domainMismatch,
    duplicateInputConnection, unknownParameter, parameterOutOfRange,
    cycleDetected, resourceLimitExceeded
};
struct Diagnostic {
    DiagnosticCode code;
    Severity severity;
    std::optional<model::NodeId> nodeId;
    std::optional<model::PortId> portId;
    std::string message;
};
} // namespace nodsynth::compiler
```

```cpp
// include/nodsynth/compiler/CompiledGraph.h
#pragma once
#include <optional>
#include <vector>
#include <nodsynth/compiler/Diagnostic.h>
#include <nodsynth/model/GraphDocument.h>
#include <nodsynth/model/NodeSchema.h>

namespace nodsynth::compiler {
struct BufferAssignment {
    model::Endpoint output;
    std::uint32_t slot;
    std::uint32_t channels;
    model::NodeScope domain;
};
struct CompiledGraph {
    struct Node {
        model::NodeRecord record;
        model::NodeScope scope;
    };
    struct Connection {
        model::Connection record;
        model::PortKind kind;
        std::uint32_t bufferSlot;
    };
    std::vector<Node> nodes;
    std::vector<Connection> connections;
    std::vector<model::NodeId> perVoiceOrder;
    std::vector<model::NodeId> globalOrder;
    std::vector<BufferAssignment> audioBuffers;
    std::vector<BufferAssignment> controlBuffers;
};
struct CompileResult { std::optional<CompiledGraph> graph; std::vector<Diagnostic> diagnostics; };
} // namespace nodsynth::compiler
```

- [ ] **Step 4: Implement endpoint, direction, type, channel, version, and fan-in validation**

```cpp
// include/nodsynth/compiler/GraphCompiler.h
#pragma once
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
} // namespace nodsynth::compiler
```

In `GraphCompiler.cpp`, build ordered maps keyed by `NodeId`; never rely on vector insertion order. For each node, require a registered type and an exact supported schema version. For each stored parameter, require a matching schema ID and a value inside its inclusive range; omitted parameters use schema defaults in the later runtime. For each connection, resolve both endpoints, require output-to-input direction, identical `PortKind`, and identical audio channel counts. Track target `Endpoint` values in a set and emit `duplicateInputConnection` for the second writer. Return all independent diagnostics in stable code/node/port order and leave `graph` empty if any error exists.

Add the compiler target at this point, after it has a real source file:

```cmake
add_library(nod_compiler src/compiler/GraphCompiler.cpp)
target_include_directories(nod_compiler PUBLIC include)
target_link_libraries(nod_compiler PUBLIC nod_model)
target_compile_features(nod_compiler PUBLIC cxx_std_20)
target_link_libraries(nod_tests PRIVATE nod_compiler)
```

- [ ] **Step 5: Add one test per diagnostic family and run them**

Add focused cases for missing endpoint, missing port, reversed direction, channel mismatch, duplicate input, unsupported version, unknown parameter, and out-of-range parameter. Every case must assert the diagnostic code plus node/port location, not English message text.

Run: `cmake --build --preset dev --target nod_tests -j2 && ctest --preset dev -R "compiler"`

Expected: all validation tests pass.

- [ ] **Step 6: Commit structured validation**

```bash
git add CMakeLists.txt include/nodsynth/compiler src/compiler tests
git commit -m "feat: validate typed node graphs"
```

---

### Task 5: Scope Boundaries, Cycle Detection, and Deterministic Scheduling

**Files:**
- Create: `src/compiler/DependencyGraph.h`
- Create: `src/compiler/DependencyGraph.cpp`
- Create: `tests/compiler/GraphSchedulingTests.cpp`
- Modify: `src/compiler/GraphCompiler.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: validated endpoints and schemas from Task 4.
- Produces: populated `CompiledGraph::perVoiceOrder/globalOrder`; latency-break nodes are represented once in their declared scope, while their incoming edge is omitted only from current-block dependency analysis.

- [ ] **Step 1: Write tests for illegal scope crossing and legal Voice Mix crossing**

```cpp
// tests/compiler/GraphSchedulingTests.cpp
TEST_CASE("per-voice output cannot feed an ordinary global input") {
    auto [registry, graph] = testGraphWithIllegalScopeCrossing();
    const auto result = GraphCompiler{}.compile(graph, registry);
    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::domainMismatch));
}

TEST_CASE("voice boundary accepts per-voice input and produces global output") {
    auto [registry, graph] = testGraphWithVoiceMix();
    const auto result = GraphCompiler{}.compile(graph, registry);
    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->perVoiceOrder == std::vector<NodeId>{NodeId{"osc"}});
    REQUIRE(result.graph->globalOrder == std::vector<NodeId>{NodeId{"mix"}, NodeId{"out"}});
}
```

The Voice Mix schema is `NodeScope::global`, with input domain `PortDomain::perVoice` and output domain `PortDomain::sameAsNode`. Resolve `sameAsNode` to the node's declared scope before comparing edge domains.

- [ ] **Step 2: Write cycle tests before implementing dependency analysis**

```cpp
TEST_CASE("ordinary feedback is rejected") {
    auto [registry, graph] = testGraphWithGainCycle(false);
    const auto result = GraphCompiler{}.compile(graph, registry);
    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::cycleDetected));
}

TEST_CASE("latency break makes feedback schedulable") {
    auto [registry, graph] = testGraphWithGainCycle(true);
    const auto result = GraphCompiler{}.compile(graph, registry);
    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->globalOrder ==
            std::vector<NodeId>{NodeId{"delay"}, NodeId{"gain"}, NodeId{"out"}});
}
```

- [ ] **Step 3: Run the focused tests and verify domain/cycle failures**

Run: `cmake --build --preset dev --target nod_tests -j2 && ctest --preset dev -R "scope|feedback|boundary"`

Expected: new tests fail because scope and dependency analysis are not implemented.

- [ ] **Step 4: Implement a deterministic dependency graph**

```cpp
// src/compiler/DependencyGraph.h
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
```

Use Kahn's algorithm with a sorted `std::set<NodeId>` for ready nodes. If the result contains fewer nodes than the graph, return an empty vector. In `GraphCompiler`, omit every incoming dependency edge whose target schema has `breaksDependencyCycle == true`; the runtime phase will later split that node into latency-read and latency-write operations.

- [ ] **Step 5: Implement domain resolution and split schedules**

Add a local function:

```cpp
static NodeScope resolveDomain(const NodeSchema& owner, const PortSchema& port) {
    switch (port.domain) {
        case PortDomain::perVoice: return NodeScope::perVoice;
        case PortDomain::global: return NodeScope::global;
        case PortDomain::sameAsNode: return owner.scope;
    }
    std::terminate();
}
```

Reject an edge when resolved source and destination domains differ. A global Voice Mix node is valid because its input explicitly resolves to `perVoice`. Build separate dependency graphs for `perVoice` and `global`; add Voice Mix itself only to the global schedule. Emit `cycleDetected` when either topological sort fails.

- [ ] **Step 6: Verify insertion-order independence**

Add a test that shuffles the same nodes and connections through ten fixed permutations, compiles each graph, and compares both output schedules to the first result.

Run: `cmake --build --preset dev --target nod_tests -j2 && ctest --preset dev -R "scheduling|feedback|boundary|deterministic"`

Expected: all scheduling tests pass.

- [ ] **Step 7: Commit graph scheduling**

```bash
git add src/compiler tests/compiler tests/CMakeLists.txt
git commit -m "feat: schedule scoped acyclic graphs"
```

---

### Task 6: Deterministic Buffer Planning and Resource Limits

**Files:**
- Create: `src/compiler/BufferPlanner.h`
- Create: `src/compiler/BufferPlanner.cpp`
- Create: `tests/compiler/BufferPlannerTests.cpp`
- Modify: `src/compiler/GraphCompiler.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: deterministic schedules from Task 5.
- Produces: `CompiledGraph::audioBuffers/controlBuffers`, assigning reusable logical slots; the realtime phase allocates physical buffers from these exact assignments.

- [ ] **Step 1: Write tests for slot reuse and resource overflow**

```cpp
// tests/compiler/BufferPlannerTests.cpp
#include <algorithm>
#include <stdexcept>

static std::uint32_t slotFor(const CompiledGraph& graph, const Endpoint& endpoint) {
    const auto found = std::ranges::find(graph.audioBuffers, endpoint, &BufferAssignment::output);
    if (found == graph.audioBuffers.end()) throw std::logic_error{"missing test buffer assignment"};
    return found->slot;
}

TEST_CASE("audio slots are reused after the last consumer") {
    auto [registry, graph] = testLinearAudioGraphWithTwoNonOverlappingTemps();
    const auto result = GraphCompiler{}.compile(graph, registry);
    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->audioBuffers.size() == 4);
    REQUIRE(slotFor(*result.graph, Endpoint{NodeId{"source-a"}, PortId{"audio"}}) ==
            slotFor(*result.graph, Endpoint{NodeId{"source-b"}, PortId{"audio"}}));
}

TEST_CASE("compiler rejects a plan above its buffer budget") {
    auto [registry, graph] = testWideAudioGraph();
    const auto result = GraphCompiler{{.maxNodes = 4096, .maxPhysicalBufferChannels = 2, .maxVoices = 16}}.compile(graph, registry);
    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::resourceLimitExceeded));
}
```

- [ ] **Step 2: Run the focused tests and verify buffer assertions fail**

Run: `cmake --build --preset dev --target nod_tests -j2 && ctest --preset dev -R "buffer"`

Expected: tests fail because compiled buffer assignments are empty.

- [ ] **Step 3: Implement liveness-based logical slot assignment**

```cpp
// src/compiler/BufferPlanner.h
#pragma once
#include <vector>
#include <nodsynth/compiler/CompiledGraph.h>
#include <nodsynth/model/NodeSchema.h>

namespace nodsynth::compiler::detail {
struct PlannedBuffers {
    std::vector<BufferAssignment> audio;
    std::vector<BufferAssignment> control;
    std::uint32_t perVoicePhysicalChannels{};
    std::uint32_t globalPhysicalChannels{};
};

PlannedBuffers planBuffers(
    const model::GraphSnapshot& graph,
    const model::SchemaRegistry& registry,
    const std::vector<model::NodeId>& perVoiceOrder,
    const std::vector<model::NodeId>& globalOrder);
} // namespace nodsynth::compiler::detail
```

The implementation must compute each Audio/Control output's production index and final consumer index. Plan PerVoice and Global schedules in separate slot namespaces. In each schedule, return expired slots to sorted free-slot sets, then allocate the lowest available slot. Audio slots may be reused only by outputs with the same channel count; Control has its own single-channel slot namespace. Gate and Note outputs are event streams and do not consume sample-buffer slots in this phase. Store the owning node's scope in each `BufferAssignment`. Sort final assignments by output endpoint before storing them so equality is insertion-order independent. Populate `CompiledGraph::nodes` and `CompiledGraph::connections` in stable endpoint order; every Audio/Control compiled connection carries its producer's assigned slot, while Gate/Note connections use slot `0` because their later runtime storage is event-based.

- [ ] **Step 4: Enforce node and buffer limits in the compiler**

Before endpoint validation, reject graphs with more than `limits_.maxNodes`. After buffer planning, compute `perVoicePhysicalChannels * limits_.maxVoices + globalPhysicalChannels` and reject a value above `limits_.maxPhysicalBufferChannels`. Emit one `resourceLimitExceeded` diagnostic with no node/port location and include the actual and allowed values in the message.

- [ ] **Step 5: Run compiler tests and the complete suite**

Run: `cmake --build --preset dev --target nod_tests -j2 && ctest --preset dev`

Expected: all tests pass, including deterministic scheduling and buffer reuse.

- [ ] **Step 6: Commit resource planning**

```bash
git add include/nodsynth/compiler src/compiler tests/compiler tests/CMakeLists.txt
git commit -m "feat: plan graph buffers and limits"
```

---

### Task 7: Runnable Graph-Check Tool and Cross-Platform Core CI

**Files:**
- Create: `apps/graphcheck/Main.cpp`
- Create: `apps/graphcheck/Scenarios.h`
- Create: `apps/graphcheck/Scenarios.cpp`
- Create: `.github/workflows/ci.yml`
- Create: `README.md`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/compiler/GraphCheckScenarioTests.cpp`

**Interfaces:**
- Consumes: complete model/compiler public API from Tasks 1–6.
- Produces: `nod_graphcheck --scenario valid|type-error|cycle`; exit code `0` for the expected scenario result and stable text output suitable for CI smoke checks.

- [ ] **Step 1: Extract built-in smoke graph builders and write scenario tests**

```cpp
// tests/compiler/GraphCheckScenarioTests.cpp
#include <catch2/catch_test_macros.hpp>
#include "../../apps/graphcheck/Scenarios.h"

TEST_CASE("valid subtractive graph compiles into both domains") {
    const auto scenario = nodsynth::graphcheck::makeScenario("valid");
    const auto result = nodsynth::compiler::GraphCompiler{}.compile(scenario.graph, scenario.registry);
    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->perVoiceOrder.size() == 5);
    REQUIRE(result.graph->globalOrder.size() == 2);
}

TEST_CASE("named invalid scenarios return their expected diagnostic") {
    REQUIRE(nodsynth::graphcheck::expectedDiagnostic("type-error") ==
            nodsynth::compiler::DiagnosticCode::portKindMismatch);
    REQUIRE(nodsynth::graphcheck::expectedDiagnostic("cycle") ==
            nodsynth::compiler::DiagnosticCode::cycleDetected);
}
```

Create `apps/graphcheck/Scenarios.h/.cpp` as a small reusable library containing literal schemas for MIDI Input, Note to Frequency, Oscillator, ADSR, Gain, Voice Mix, and Audio Output. The valid graph must exercise Note, Gate, Control, Audio, PerVoice, and Global semantics.

- [ ] **Step 2: Run the test and verify scenario symbols are missing**

Run: `cmake --build --preset dev --target nod_tests -j2`

Expected: compilation fails because `Scenarios.h` does not exist.

- [ ] **Step 3: Implement the command-line contract**

```cpp
// apps/graphcheck/Main.cpp
#include "Scenarios.h"
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view{argv[1]} != "--scenario") {
        std::cerr << "usage: nod_graphcheck --scenario valid|type-error|cycle\n";
        return 64;
    }
    const auto scenario = nodsynth::graphcheck::makeScenario(argv[2]);
    const auto result = nodsynth::compiler::GraphCompiler{}.compile(scenario.graph, scenario.registry);
    if (scenario.expectSuccess) {
        if (!result.graph) return 1;
        std::cout << "valid perVoice=" << result.graph->perVoiceOrder.size()
                  << " global=" << result.graph->globalOrder.size() << '\n';
        return 0;
    }
    if (result.graph || !nodsynth::graphcheck::containsExpectedDiagnostic(result, scenario)) return 1;
    std::cout << "rejected code=" << nodsynth::graphcheck::diagnosticName(scenario.expectedCode) << '\n';
    return 0;
}
```

Add a `nod_graphcheck_scenarios` library, link it to both the CLI and tests, and register three CTest smoke cases with exact expected success exit codes.

- [ ] **Step 4: Add a three-platform core CI matrix**

```yaml
# .github/workflows/ci.yml
name: core-ci
on:
  push:
  pull_request:
jobs:
  core:
    strategy:
      matrix:
        os: [ubuntu-latest, windows-latest, macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      # actions/checkout v4.2.2
      - uses: actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683
      - name: Show toolchain
        run: cmake --version && ninja --version
      - name: Configure
        run: cmake --preset dev
      - name: Build
        run: cmake --build --preset dev --config Debug -j 2
      - name: Test
        run: ctest --preset dev -C Debug
```

- [ ] **Step 5: Document exact developer commands and phase boundary**

````markdown
# NodSynth

NodSynth is a node-based polyphonic software synthesizer. The current milestone implements the platform-independent graph model and compiler.

## Build and test

```sh
cmake --preset dev
cmake --build --preset dev -j2
ctest --preset dev
./build/dev/nod_graphcheck --scenario valid
./build/dev/nod_graphcheck --scenario type-error
./build/dev/nod_graphcheck --scenario cycle
```

The realtime audio engine and JUCE application are intentionally outside this milestone; see the approved MVP design under `docs/superpowers/specs/`.
````

- [ ] **Step 6: Run the complete local acceptance sequence**

Run:

```bash
cmake --preset dev
cmake --build --preset dev -j2
ctest --preset dev
./build/dev/nod_graphcheck --scenario valid
./build/dev/nod_graphcheck --scenario type-error
./build/dev/nod_graphcheck --scenario cycle
```

Expected output includes:

```text
100% tests passed
valid perVoice=5 global=2
rejected code=port-kind-mismatch
rejected code=cycle-detected
```

- [ ] **Step 7: Commit the first independently runnable milestone**

```bash
git add CMakeLists.txt apps .github README.md tests
git commit -m "feat: add graph compiler smoke tool"
```

## Phase 1 Completion Gate

Before starting the realtime DSP plan, verify all of the following from a clean build directory:

- `ctest --preset dev` passes every model and compiler test.
- The three graphcheck scenarios print the exact expected summaries and exit zero.
- Public `nod_model` and `nod_compiler` headers contain no JUCE or platform includes.
- Reordering nodes/connections does not change schedules or buffer assignments.
- Every error path used by the smoke scenarios includes structured code and location.
- `git status --short` is empty after the final task commit.
