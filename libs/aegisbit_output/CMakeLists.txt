add_library(aegisbit_output STATIC
  src/Output.cpp
)

set_target_properties(aegisbit_output PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(aegisbit_output
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(aegisbit_output
  PUBLIC
    amdgpu_rewrite_core
)

if(NEW_AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
