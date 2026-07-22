add_executable(nod_tests
  compiler/BufferPlannerTests.cpp
  compiler/GraphCheckScenarioTests.cpp
  compiler/GraphSchedulingTests.cpp
  compiler/GraphValidationTests.cpp
  model/BuildInfoTests.cpp
  model/GraphEditorTests.cpp
  model/SchemaRegistryTests.cpp
)
target_link_libraries(nod_tests PRIVATE nod_model nod_graphcheck_scenarios Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(nod_tests)

if(NODSYNTH_BUILD_GRAPHCHECK)
  add_test(NAME graphcheck.valid COMMAND nod_graphcheck --scenario valid)
  add_test(NAME graphcheck.type-error COMMAND nod_graphcheck --scenario type-error)
  add_test(NAME graphcheck.cycle COMMAND nod_graphcheck --scenario cycle)
  add_test(
    NAME graphcheck.unknown-scenario
    COMMAND
      ${CMAKE_COMMAND}
      "-DGRAPHCHECK_EXECUTABLE=$<TARGET_FILE:nod_graphcheck>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GraphCheckUnknownScenario.cmake"
  )
endif()
