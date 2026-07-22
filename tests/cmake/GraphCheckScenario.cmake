foreach(required_variable GRAPHCHECK_EXECUTABLE SCENARIO EXPECTED_RESULT EXPECTED_STDOUT EXPECTED_STDERR)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "Missing required variable ${required_variable}.")
  endif()
endforeach()

set(expected_stdout "")
if(NOT "${EXPECTED_STDOUT}" STREQUAL "")
  set(expected_stdout "${EXPECTED_STDOUT}\n")
endif()

set(expected_stderr "")
if(NOT "${EXPECTED_STDERR}" STREQUAL "")
  set(expected_stderr "${EXPECTED_STDERR}\n")
endif()

execute_process(
  COMMAND "${GRAPHCHECK_EXECUTABLE}" --scenario "${SCENARIO}"
  RESULT_VARIABLE actual_result
  OUTPUT_VARIABLE actual_stdout
  ERROR_VARIABLE actual_stderr
)

if(NOT "${actual_result}" STREQUAL "${EXPECTED_RESULT}")
  message(FATAL_ERROR "Expected exit code ${EXPECTED_RESULT}, got '${actual_result}'.")
endif()

string(REPLACE "\r\n" "\n" normalized_stdout "${actual_stdout}")
if(NOT "${normalized_stdout}" STREQUAL "${expected_stdout}")
  message(FATAL_ERROR "Unexpected stdout: '${actual_stdout}'.")
endif()

string(REPLACE "\r\n" "\n" normalized_stderr "${actual_stderr}")
if(NOT "${normalized_stderr}" STREQUAL "${expected_stderr}")
  message(FATAL_ERROR "Unexpected stderr: '${actual_stderr}'.")
endif()
