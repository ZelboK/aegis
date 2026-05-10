add_executable(aegisbit_output_gtest
  OutputGTest.cpp
  BoundaryGTest.cpp
)

target_link_libraries(aegisbit_output_gtest
  PRIVATE
    aegisbit_output
    GTest::gtest_main
)

target_compile_definitions(aegisbit_output_gtest
  PRIVATE
    NEW_AEGIS_AEGISBIT_OUTPUT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_test(NAME AegisbitOutputGTest COMMAND aegisbit_output_gtest)
