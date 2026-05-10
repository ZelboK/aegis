add_library(amdgpu_code_object INTERFACE)

target_include_directories(amdgpu_code_object
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(amdgpu_code_object
  INTERFACE
    amdgpu_instr_backend
)

if(NEW_AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
