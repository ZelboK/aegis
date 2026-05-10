add_library(amdgpu_pc_reloc STATIC
  src/BranchMath.cpp
  src/PatchPlanner.cpp
  src/Relocator.cpp
)

set_target_properties(amdgpu_pc_reloc PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(amdgpu_pc_reloc
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(amdgpu_pc_reloc
  PUBLIC
    amdgpu_instr_backend
)

if(NEW_AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
