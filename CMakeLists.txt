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

if(NODSYNTH_BUILD_TESTS)
  include(CTest)
  include(cmake/Dependencies.cmake)
  add_subdirectory(tests)
  target_link_libraries(nod_tests PRIVATE nod_compiler)
endif()
