add_library(amdgpu_rewrite_core STATIC
  src/RewriteCore.cpp
)

set_target_properties(amdgpu_rewrite_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(amdgpu_rewrite_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(amdgpu_rewrite_core
  PUBLIC
    amdgpu_instr_backend
    amdgpu_pc_reloc
)

if(NEW_AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
