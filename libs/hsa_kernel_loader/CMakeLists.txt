add_library(hsa_kernel_loader INTERFACE)

target_include_directories(hsa_kernel_loader
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hsa_kernel_loader
  INTERFACE
    amdgpu_instr_backend
)

if(AEGIS_HAS_HSA_INTERCEPT)
  add_library(hsa_kernel_loader_hsa STATIC
    src/HsaKernelLoader.cpp
  )

  set_target_properties(hsa_kernel_loader_hsa PROPERTIES
    POSITION_INDEPENDENT_CODE ON
  )

  target_include_directories(hsa_kernel_loader_hsa
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
  )

  target_include_directories(hsa_kernel_loader_hsa
    SYSTEM PRIVATE
      ${HSA_INCLUDE_DIR}
  )

  target_link_libraries(hsa_kernel_loader_hsa
    PUBLIC
      hsa_kernel_loader
    PRIVATE
      ${HSA_RUNTIME_LIBRARY}
  )
endif()

if(NEW_AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
