add_executable(amdgpu_code_object_llvm_gtest
  LlvmCodeObjectParserGTest.cpp
)

target_link_libraries(amdgpu_code_object_llvm_gtest
  PRIVATE
    amdgpu_code_object_llvm
    GTest::gtest_main
)

target_compile_definitions(amdgpu_code_object_llvm_gtest
  PRIVATE
    NEW_AEGIS_AMDGPU_CODE_OBJECT_LLVM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_test(NAME AmdgpuCodeObjectLlvmGTest COMMAND amdgpu_code_object_llvm_gtest)
