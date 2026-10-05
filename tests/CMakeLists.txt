add_executable(nod_tests
  compiler/BufferPlannerTests.cpp
  compiler/GraphCheckScenarioTests.cpp
  compiler/GraphSchedulingTests.cpp
  compiler/GraphValidationTests.cpp
  model/BuildInfoTests.cpp
  model/GraphEditorTests.cpp
  model/SchemaRegistryTests.cpp
  model/LayoutTests.cpp
  midi/SmfTests.cpp
  persist/ProjectFileTests.cpp
  render/OfflineRendererTests.cpp
  runtime/RuntimeTests.cpp
  song/SongTests.cpp
  daw/TimelineTests.cpp
)
target_link_libraries(nod_tests PRIVATE nod_model nod_nodes nod_persist nod_midi nod_render_core nod_song nod_daw_core nod_graphcheck_scenarios Catch2::Catch2WithMain)
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/external/fluidsynth/fluidsynth-v2.6.0-win10-x64-cpp11/bin/fluidsynth.exe" NOD_FLUIDSYNTH_PATH)
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/external/VintageDreamsWaves-v2.sf2" NOD_SOUNDFONT_PATH)
add_executable(nod_model_adapter song/ModelAdapterMain.cpp)
target_link_libraries(nod_model_adapter PRIVATE nod_persist)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/presets" NOD_PRESETS_ROOT)
target_compile_definitions(nod_tests PRIVATE "NOD_RENDER_EXECUTABLE=\"$<TARGET_FILE:nod_render>\"" "NOD_EXECUTABLE=\"$<TARGET_FILE:nod>\"" "NOD_FLUIDSYNTH=\"${NOD_FLUIDSYNTH_PATH}\"" "NOD_SOUNDFONT=\"${NOD_SOUNDFONT_PATH}\"" "NOD_MODEL_ADAPTER=\"$<TARGET_FILE:nod_model_adapter>\"" "NOD_PRESETS_ROOT=\"${NOD_PRESETS_ROOT}\"")
add_dependencies(nod_tests nod_render nod nod_model_adapter)
if(TARGET nod_vst3_worker)
  target_compile_definitions(nod_tests PRIVATE "NOD_VST3_WORKER=\"$<TARGET_FILE:nod_vst3_worker>\"" "NOD_VST3_PLUGIN=\"$<TARGET_FILE:nodsynth_vst3>\"")
  add_dependencies(nod_tests nod_vst3_worker nodsynth_vst3)
  if(TARGET nod_latency_probe)
    target_compile_definitions(nod_tests PRIVATE "NOD_LATENCY_PLUGIN=\"$<TARGET_FILE:nod_latency_probe>\"")
    add_dependencies(nod_tests nod_latency_probe)
  endif()
endif()
include(Catch)
catch_discover_tests(nod_tests)

add_executable(nod_alloc_probe runtime/AllocProbe.cpp)
target_link_libraries(nod_alloc_probe PRIVATE nod_nodes)
add_test(NAME runtime.no-allocation COMMAND nod_alloc_probe)

if(NODSYNTH_BUILD_GRAPHCHECK)
  function(add_graphcheck_scenario_test name scenario expected_result expected_stdout expected_stderr)
    add_test(
      NAME "graphcheck.${name}"
      COMMAND
        ${CMAKE_COMMAND}
        "-DGRAPHCHECK_EXECUTABLE=$<TARGET_FILE:nod_graphcheck>"
        "-DSCENARIO=${scenario}"
        "-DEXPECTED_RESULT=${expected_result}"
        "-DEXPECTED_STDOUT=${expected_stdout}"
        "-DEXPECTED_STDERR=${expected_stderr}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GraphCheckScenario.cmake"
    )
  endfunction()

  add_graphcheck_scenario_test(valid valid 0 "valid perVoice=5 global=2" "")
  add_graphcheck_scenario_test(type-error type-error 0 "rejected code=port-kind-mismatch" "")
  add_graphcheck_scenario_test(cycle cycle 0 "rejected code=cycle-detected" "")
  add_graphcheck_scenario_test(
    unknown-scenario unknown 64 "" "usage: nod_graphcheck --scenario valid|type-error|cycle"
  )
endif()
