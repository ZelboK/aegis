add_library(hsa_kernel_loader INTERFACE)

target_include_directories(hsa_kernel_loader
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hsa_kernel_loader
  INTERFACE
    amdgpu_instr_backend
)

if(NEW_AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
