set(expected_stderr "usage: nod_graphcheck --scenario valid|type-error|cycle\n")

execute_process(
  COMMAND "${GRAPHCHECK_EXECUTABLE}" --scenario unknown
  RESULT_VARIABLE actual_result
  OUTPUT_VARIABLE actual_stdout
  ERROR_VARIABLE actual_stderr
)

if(NOT "${actual_result}" STREQUAL "64")
  message(FATAL_ERROR "Expected exit code 64, got '${actual_result}'.")
endif()

if(NOT "${actual_stdout}" STREQUAL "")
  message(FATAL_ERROR "Expected empty stdout, got '${actual_stdout}'.")
endif()

string(REPLACE "\r\n" "\n" normalized_stderr "${actual_stderr}")
if(NOT "${normalized_stderr}" STREQUAL "${expected_stderr}")
  message(FATAL_ERROR "Unexpected stderr: '${actual_stderr}'.")
endif()
