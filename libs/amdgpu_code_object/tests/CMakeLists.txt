add_executable(amdgpu_code_object_gtest
  BoundaryGTest.cpp
)

target_link_libraries(amdgpu_code_object_gtest
  PRIVATE
    amdgpu_code_object
    GTest::gtest_main
)

target_compile_definitions(amdgpu_code_object_gtest
  PRIVATE
    NEW_AEGIS_AMDGPU_CODE_OBJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_test(NAME AmdgpuCodeObjectGTest COMMAND amdgpu_code_object_gtest)
