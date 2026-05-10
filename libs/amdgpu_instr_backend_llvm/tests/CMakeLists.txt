add_executable(amdgpu_instr_backend_llvm_gtest
  LlvmBackendGTest.cpp
)

target_link_libraries(amdgpu_instr_backend_llvm_gtest
  PRIVATE
    amdgpu_pc_reloc
    amdgpu_instr_backend_llvm
    GTest::gtest_main
)

target_compile_definitions(amdgpu_instr_backend_llvm_gtest
  PRIVATE
    NEW_AEGIS_AMDGPU_INSTR_BACKEND_LLVM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_test(NAME AmdgpuInstrBackendLlvmGTest
         COMMAND amdgpu_instr_backend_llvm_gtest)
