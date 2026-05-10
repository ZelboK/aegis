add_library(amdgpu_instr_backend INTERFACE)

target_include_directories(amdgpu_instr_backend
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
