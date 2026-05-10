add_executable(amdgpu_rewrite_core_gtest
  RewriteCoreGTest.cpp
  BoundaryGTest.cpp
)

target_link_libraries(amdgpu_rewrite_core_gtest
  PRIVATE
    amdgpu_rewrite_core
    GTest::gtest_main
)

target_compile_definitions(amdgpu_rewrite_core_gtest
  PRIVATE
    NEW_AEGIS_AMDGPU_REWRITE_CORE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_test(NAME AmdgpuRewriteCoreGTest COMMAND amdgpu_rewrite_core_gtest)
