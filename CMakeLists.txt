cmake_minimum_required(VERSION 3.27)
project(NodSynth VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NODSYNTH_BUILD_TESTS "Build NodSynth tests" ON)
option(NODSYNTH_BUILD_GRAPHCHECK "Build the graph-checking CLI" ON)

add_library(nod_model
  src/model/BuildInfo.cpp
  src/model/GraphDocument.cpp
  src/model/GraphEditor.cpp
  src/model/SchemaRegistry.cpp
)
target_include_directories(nod_model PUBLIC include)
target_compile_features(nod_model PUBLIC cxx_std_20)

add_library(nod_compiler
  src/compiler/BufferPlanner.cpp
  src/compiler/DependencyGraph.cpp
  src/compiler/GraphCompiler.cpp
)
target_include_directories(nod_compiler PUBLIC include)
target_link_libraries(nod_compiler PUBLIC nod_model)
target_compile_features(nod_compiler PUBLIC cxx_std_20)

add_library(nod_runtime
  src/runtime/Engine.cpp
  src/runtime/GraphSession.cpp
  src/runtime/PrepareConfig.cpp
  src/runtime/RuntimePlan.cpp
  src/runtime/VoiceAllocator.cpp
)
target_include_directories(nod_runtime PUBLIC include)
target_link_libraries(nod_runtime PUBLIC nod_compiler)
target_compile_features(nod_runtime PUBLIC cxx_std_20)

add_library(nod_nodes
  src/nodes/BuiltinNodes.cpp
  src/nodes/DspNodes.cpp
  src/nodes/Patches.cpp
)
target_include_directories(nod_nodes PUBLIC include)
target_link_libraries(nod_nodes PUBLIC nod_runtime)
target_compile_features(nod_nodes PUBLIC cxx_std_20)

add_library(nod_persist
  src/persist/Json.cpp
  src/persist/ProjectFile.cpp
)
target_include_directories(nod_persist PUBLIC include)
target_link_libraries(nod_persist PUBLIC nod_model)
target_compile_features(nod_persist PUBLIC cxx_std_20)

add_executable(nod_render apps/render/Main.cpp)
target_link_libraries(nod_render PRIVATE nod_nodes nod_persist)

if(WIN32)
  add_executable(nodsynth_app WIN32 apps/win32/Main.cpp apps/win32/AudioHost.cpp)
  target_link_libraries(nodsynth_app PRIVATE nod_nodes nod_persist ole32 uuid winmm avrt propsys comdlg32 shell32 user32 gdi32)
  target_link_options(nodsynth_app PRIVATE -municode)
endif()

if(NODSYNTH_BUILD_GRAPHCHECK OR NODSYNTH_BUILD_TESTS)
  add_library(nod_graphcheck_scenarios apps/graphcheck/Scenarios.cpp)
  target_include_directories(nod_graphcheck_scenarios PUBLIC apps/graphcheck)
  target_link_libraries(nod_graphcheck_scenarios PUBLIC nod_compiler nod_nodes)
  target_compile_features(nod_graphcheck_scenarios PUBLIC cxx_std_20)
endif()

if(NODSYNTH_BUILD_GRAPHCHECK)
  add_executable(nod_graphcheck apps/graphcheck/Main.cpp)
  target_link_libraries(nod_graphcheck PRIVATE nod_graphcheck_scenarios)
endif()

if(NODSYNTH_BUILD_TESTS)
  include(CTest)
  include(cmake/Dependencies.cmake)
  add_subdirectory(tests)
  target_link_libraries(nod_tests PRIVATE nod_compiler)
  add_test(NAME render.smoke COMMAND nod_render --output "${CMAKE_BINARY_DIR}/smoke.wav" --sample-rate 48000 --block-size 128)
endif()
