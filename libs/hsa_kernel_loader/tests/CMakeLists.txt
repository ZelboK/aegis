add_executable(hsa_kernel_loader_gtest
  BoundaryGTest.cpp
)

target_link_libraries(hsa_kernel_loader_gtest
  PRIVATE
    hsa_kernel_loader
    GTest::gtest_main
)

target_compile_definitions(hsa_kernel_loader_gtest
  PRIVATE
    NEW_AEGIS_HSA_KERNEL_LOADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_test(NAME HsaKernelLoaderGTest COMMAND hsa_kernel_loader_gtest)

if(TARGET hsa_kernel_loader_hsa)
  add_executable(hsa_kernel_loader_hsa_smoke_gtest
    HsaKernelLoaderHsaSmokeGTest.cpp
  )

  target_link_libraries(hsa_kernel_loader_hsa_smoke_gtest
    PRIVATE
      hsa_kernel_loader_hsa
      GTest::gtest_main
  )

  add_test(NAME HsaKernelLoaderHsaSmokeGTest
           COMMAND hsa_kernel_loader_hsa_smoke_gtest)
endif()
