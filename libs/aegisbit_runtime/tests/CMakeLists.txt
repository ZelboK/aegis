add_executable(aegisbit_runtime_gtest
  RuntimeGTest.cpp
  BoundaryGTest.cpp
)

target_link_libraries(aegisbit_runtime_gtest
  PRIVATE
    aegisbit_runtime
    GTest::gtest_main
)

target_compile_definitions(aegisbit_runtime_gtest
  PRIVATE
    NEW_AEGIS_AEGISBIT_RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_test(NAME AegisbitRuntimeGTest COMMAND aegisbit_runtime_gtest)
