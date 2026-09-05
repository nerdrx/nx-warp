# Decoder pass tests.  Each pass is guarded on EXISTS so either agent can
# create this file identically and a missing sibling never breaks configure.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/passA/CMakeLists.txt)
  add_subdirectory(passA)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/passB/CMakeLists.txt)
  add_subdirectory(passB)
endif()
# The exactness proof behind Pass B's UNORM store; links no decoder code.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unorm/CMakeLists.txt)
  add_subdirectory(unorm)
endif()
# End-to-end tests for the decoder library that drives both passes.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/conformance/CMakeLists.txt)
  add_subdirectory(conformance)
endif()
