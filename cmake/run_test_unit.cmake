include(${CMAKE_CURRENT_LIST_DIR}/skymp_execute_process.cmake)

# Tags were added to skymp20 to merge coverage reports
# Unused currently, so always use 00000 as a tag and clear temp files

# Important note: 
# In tests added by subdirectories, CMAKE_BINARY_DIR refers to subdirectory binary dir instead of the global one

set(tag 00000)
file(REMOVE_RECURSE ${CMAKE_BINARY_DIR}/coverage_tag_00000)
file(GLOB temp_coverage "${CMAKE_BINARY_DIR}/coverage_*.tmp")
if(temp_coverage)
  file(REMOVE ${temp_coverage})
endif()

# The dist under test is also the integration tests' server directory, and
# CI's build tree is one cache shared across builds of every branch: a server
# of an earlier run leaves its world there (world/load-order.json at every
# load since thuum ADR-043), which DistContentsTest would flag as an excess
# dist file, and the integration tests that would clean it never run after a
# failed test_unit (FIXTURES_REQUIRED unit_passed). A world is runtime state,
# no part of the dist: drop it before the unit run.
if(UNIT_WORKING_DIRECTORY)
  file(REMOVE_RECURSE "${UNIT_WORKING_DIRECTORY}/dist/server/world")
endif()

# Emscripten support
if("${EXE_PATH}" MATCHES ".*\\.js")
  execute_process(COMMAND node ${EXE_PATH}
    RESULT_VARIABLE res
    OUTPUT_VARIABLE out
    WORKING_DIRECTORY "${UNIT_WORKING_DIRECTORY}"
  )
else()
  execute_process(COMMAND ${EXE_PATH}
    RESULT_VARIABLE res
    OUTPUT_VARIABLE out
    WORKING_DIRECTORY "${UNIT_WORKING_DIRECTORY}"
  )
endif()

message("${out}")

if(NOT "${res}" STREQUAL "0")
  message(FATAL_ERROR "Bad exit status ${res}")
endif()

# OpenCppCoverage is Windows-only
if(WIN32 AND CPPCOV)
  skymp_execute_process(
    EXECUTABLE_PATH ${EXE_PATH}
    CPPCOV ${CPPCOV}
    CPPCOV_TAG ${tag}
    CPPCOV_PATH ${CPPCOV_PATH}
    CPPCOV_MODULES *
    CPPCOV_SOURCES *
    CPPCOV_OUTPUT_DIRECTORY ${COVERAGE_HTML_OUT_DIR}
    OUT_EXIT_CODE EXIT_CODE
    OUT_STDOUT STDOUT
  )

  if(NOT "${EXIT_CODE}" STREQUAL "0")
    message(FATAL_ERROR "Bad exit status ${EXIT_CODE} ${STDOUT}")
  endif()
endif()
