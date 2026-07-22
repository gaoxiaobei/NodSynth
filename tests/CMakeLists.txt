add_executable(nod_tests
  compiler/GraphValidationTests.cpp
  model/BuildInfoTests.cpp
  model/GraphEditorTests.cpp
  model/SchemaRegistryTests.cpp
)
target_link_libraries(nod_tests PRIVATE nod_model Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(nod_tests)
