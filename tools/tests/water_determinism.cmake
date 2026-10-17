# Cross-process determinism of the water swell (ctest water_determinism): two runs of
# `test_water --hash` must print the same fingerprint.
execute_process(COMMAND ${EXE} --hash OUTPUT_VARIABLE A RESULT_VARIABLE RA OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${EXE} --hash OUTPUT_VARIABLE B RESULT_VARIABLE RB OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT RA EQUAL 0 OR NOT RB EQUAL 0 OR "${A}" STREQUAL "")
  message(FATAL_ERROR "water_determinism: test_water --hash failed (${RA} ${RB})")
endif()
if(NOT "${A}" STREQUAL "${B}")
  message(FATAL_ERROR "water_determinism: process hashes differ: ${A} vs ${B}")
endif()
message(STATUS "water_determinism: ${A} in both processes")
