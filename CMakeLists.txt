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

add_library(nod_midi
  src/midi/Smf.cpp
)
target_include_directories(nod_midi PUBLIC include)
target_compile_features(nod_midi PUBLIC cxx_std_20)

add_library(nod_persist
  src/persist/Json.cpp
  src/persist/ProjectFile.cpp
)
target_include_directories(nod_persist PUBLIC include)
target_link_libraries(nod_persist PUBLIC nod_model)
target_compile_features(nod_persist PUBLIC cxx_std_20)

add_library(nod_render_core
  src/render/OfflineRenderer.cpp
)
target_include_directories(nod_render_core PUBLIC include)
target_link_libraries(nod_render_core PUBLIC nod_nodes nod_persist nod_midi)
target_compile_features(nod_render_core PUBLIC cxx_std_20)

add_executable(nod_render apps/render/Main.cpp)
target_link_libraries(nod_render PRIVATE nod_render_core)

add_library(nod_song
  src/song/SongDocument.cpp
  src/song/SongQuery.cpp
  src/song/MidiSong.cpp
  src/song/Process.cpp
  src/song/AudioAnalysis.cpp
  src/song/SongRenderer.cpp
  src/song/ModelClient.cpp
  src/song/Presets.cpp
  src/song/Resources.cpp
  src/song/SongDiff.cpp
  src/song/Automation.cpp
  src/song/AudioExport.cpp
  src/song/Phrases.cpp
)
target_include_directories(nod_song PUBLIC include)
target_link_libraries(nod_song PUBLIC nod_render_core)
target_compile_definitions(nod_song PRIVATE "NOD_BUILTIN_PRESETS=\"${CMAKE_SOURCE_DIR}/presets\"")
target_compile_features(nod_song PUBLIC cxx_std_20)

add_executable(nod apps/nod/Main.cpp)
target_link_libraries(nod PRIVATE nod_song)

add_library(nod_daw_core src/daw/Timeline.cpp)
target_include_directories(nod_daw_core PUBLIC include)
target_link_libraries(nod_daw_core PUBLIC nod_song)
target_compile_features(nod_daw_core PUBLIC cxx_std_20)

if(WIN32)
  add_executable(nodsynth_app WIN32 apps/win32/Main.cpp apps/win32/AudioHost.cpp)
  target_link_libraries(nodsynth_app PRIVATE nod_nodes nod_persist ole32 uuid winmm avrt propsys comdlg32 shell32 user32 gdi32)
  target_link_options(nodsynth_app PRIVATE -municode)

  add_executable(nod_daw WIN32 apps/daw/Main.cpp apps/win32/AudioHost.cpp)
  target_link_libraries(nod_daw PRIVATE nod_daw_core nod_song ole32 uuid winmm avrt propsys comdlg32 shell32 user32 gdi32)
  target_link_options(nod_daw PRIVATE -municode)
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

option(NODSYNTH_BUILD_VST3 "Build the NodSynth VST3 instrument" ON)
if(NODSYNTH_BUILD_VST3 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/CMakeLists.txt")
  set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
  set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
  set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/vst3sdk ${CMAKE_BINARY_DIR}/vst3sdk EXCLUDE_FROM_ALL)
  set(SMTG_MDA_VST3_VST2_COMPATIBLE OFF CACHE BOOL "" FORCE)
  set(vstsdk_VERSION 3.8.1)
  function(smtg_target_setup_as_vst3_example target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_link_libraries(${target} PRIVATE sdk)
    if(COMMAND smtg_target_setup_options)
      smtg_target_setup_options(${target}
        BUNDLE_IDENTIFIER "com.steinberg.vst3.${target}"
        COMPANY_NAME "Steinberg Media Technologies")
    endif()
  endfunction()
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/public.sdk/samples/vst/mda-vst3" ${CMAKE_BINARY_DIR}/mda-vst3 EXCLUDE_FROM_ALL)
  if(TARGET mda-vst3)
    smtg_target_configure_version_file(mda-vst3)
    target_link_libraries(mda-vst3 PRIVATE sdk)
    target_link_options(mda-vst3 PRIVATE "-Wl,-Bstatic" "-lc++" "-lunwind" "-Wl,-Bdynamic")
    set_target_properties(mda-vst3 PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/mda-vst3.vst3/Contents/x86_64-win"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/mda-vst3.vst3/Contents/x86_64-win")
  endif()
  add_library(nod_host
    src/host/Vst3Host.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/public.sdk/source/vst/hosting/module_win32.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/public.sdk/source/vst/hosting/parameterchanges.cpp")
  target_include_directories(nod_host PUBLIC include "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk")
  target_link_libraries(nod_host PUBLIC sdk_hosting sdk)
  target_compile_features(nod_host PUBLIC cxx_std_20)
  add_executable(nod_vst3_worker apps/host/Worker.cpp)
  target_link_libraries(nod_vst3_worker PRIVATE nod_host nod_midi nod_persist)
  target_compile_features(nod_vst3_worker PRIVATE cxx_std_20)
  smtg_add_vst3plugin(nodsynth_vst3
    apps/vst3/Processor.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/public.sdk/source/vst/vstsinglecomponenteffect.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/public.sdk/source/main/dllmain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/public.sdk/source/main/moduleinit.cpp")
  target_link_libraries(nodsynth_vst3 PRIVATE sdk nod_nodes nod_persist)
  target_link_options(nodsynth_vst3 PRIVATE "-Wl,-Bstatic" "-lc++" "-lunwind" "-Wl,-Bdynamic")
  target_compile_features(nodsynth_vst3 PRIVATE cxx_std_20)
  set_target_properties(nodsynth_vst3 PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/nodsynth_vst3.vst3/Contents/x86_64-win"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/nodsynth_vst3.vst3/Contents/x86_64-win")
  smtg_add_vst3plugin(nod_latency_probe
    tests/vst3/LatencyProbe.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/public.sdk/source/vst/vstsinglecomponenteffect.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/public.sdk/source/main/dllmain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk/public.sdk/source/main/moduleinit.cpp")
  target_link_libraries(nod_latency_probe PRIVATE sdk)
  target_link_options(nod_latency_probe PRIVATE "-Wl,-Bstatic" "-lc++" "-lunwind" "-Wl,-Bdynamic")
  target_compile_features(nod_latency_probe PRIVATE cxx_std_20)
  set_target_properties(nod_latency_probe PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/nod_latency_probe.vst3/Contents/x86_64-win"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/nod_latency_probe.vst3/Contents/x86_64-win")
  if(NODSYNTH_BUILD_TESTS)
    add_executable(nod_vst3_host_probe tests/vst3/HostProbe.cpp)
    target_link_libraries(nod_vst3_host_probe PRIVATE nod_host)
    target_compile_definitions(nod_vst3_host_probe PRIVATE "NOD_VST3_PATH=\"$<TARGET_FILE:nodsynth_vst3>\"")
    add_dependencies(nod_vst3_host_probe nodsynth_vst3)
    add_executable(nod_vst3_probe tests/vst3/PluginProbe.cpp)
    target_include_directories(nod_vst3_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk")
    target_link_libraries(nod_vst3_probe PRIVATE pluginterfaces)
    target_compile_features(nod_vst3_probe PRIVATE cxx_std_20)
    target_compile_definitions(nod_vst3_probe PRIVATE "NOD_VST3_PATH=\"$<TARGET_FILE:nodsynth_vst3>\"")
    add_dependencies(nod_vst3_probe nodsynth_vst3)
  endif()
endif()

if(NODSYNTH_BUILD_TESTS)
  include(CTest)
  if(TARGET nod_vst3_probe)
    add_test(NAME vst3.instrument COMMAND nod_vst3_probe)
  endif()
  if(TARGET nod_vst3_host_probe)
    add_test(NAME vst3.host COMMAND nod_vst3_host_probe)
    add_test(NAME vst3.crash COMMAND nod_vst3_host_probe --crash-check)
    if(TARGET mda-vst3)
      add_test(NAME vst3.mda-delay COMMAND nod_vst3_host_probe $<TARGET_FILE:mda-vst3> "mda Delay")
    endif()
    if(EXISTS "C:/Program Files/Common Files/VST3/Serum2.vst3")
      add_test(NAME vst3.serum COMMAND nod_vst3_host_probe "C:/Program Files/Common Files/VST3/Serum2.vst3")
    endif()
  endif()
  include(cmake/Dependencies.cmake)
  add_subdirectory(tests)
  target_link_libraries(nod_tests PRIVATE nod_compiler)
  add_test(NAME render.smoke COMMAND nod_render --output "${CMAKE_BINARY_DIR}/smoke.wav" --sample-rate 48000 --block-size 128)
endif()
