add_executable(hsa_intercept_gtest
  HsaInterceptGTest.cpp
)

target_link_libraries(hsa_intercept_gtest
  PRIVATE
    aegis_hsa_intercept
    GTest::gtest_main
)

target_compile_definitions(hsa_intercept_gtest
  PRIVATE
    NEW_AEGIS_HSA_INTERCEPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_test(NAME HsaInterceptGTest COMMAND hsa_intercept_gtest)
