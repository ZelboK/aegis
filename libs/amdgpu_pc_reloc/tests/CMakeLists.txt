add_executable(amdgpu_pc_reloc_core_gtest
  BranchMathGTest.cpp
  BoundaryGTest.cpp
)

target_link_libraries(amdgpu_pc_reloc_core_gtest
  PRIVATE
    amdgpu_pc_reloc
    GTest::gtest_main
)

target_compile_definitions(amdgpu_pc_reloc_core_gtest
  PRIVATE
    NEW_AEGIS_AMDGPU_PC_RELOC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
    NEW_AEGIS_AMDGPU_INSTR_BACKEND_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../amdgpu_instr_backend"
)

add_test(NAME AmdgpuPCRelocCoreGTest COMMAND amdgpu_pc_reloc_core_gtest)
