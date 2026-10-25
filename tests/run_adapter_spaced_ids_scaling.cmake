# The lane's row supplies its library-profile gate through the environment;
# forward the real executable's diagnostics and propagate every failure.
if(NOT DEFINED CR347_SCALING_EXECUTABLE)
    message(FATAL_ERROR "CR347_SCALING_EXECUTABLE is required")
endif()
execute_process(COMMAND "${CR347_SCALING_EXECUTABLE}" --row spaced RESULT_VARIABLE result)
if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR "CR347 spaced-id scaling failed: ${result}")
endif()
