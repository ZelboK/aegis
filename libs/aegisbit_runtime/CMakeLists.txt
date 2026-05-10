add_library(aegisbit_runtime STATIC
  src/Runtime.cpp
)

set_target_properties(aegisbit_runtime PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(aegisbit_runtime
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(aegisbit_runtime
  PUBLIC
    aegis_hsa_intercept
    amdgpu_rewrite_core
    aegisbit_output
)

if(NEW_AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
