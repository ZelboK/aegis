set(AMDGPU_INSTR_BACKEND_LLVM_LINK_COMPONENTS
  Core
  Support
  MC
  MCDisassembler
  MCParser
  Target
  AMDGPUInfo
  AMDGPUDesc
  AMDGPUUtils
  AMDGPUDisassembler
  AMDGPUAsmParser
  AMDGPUCodeGen
)

llvm_map_components_to_libnames(amdgpu_instr_backend_llvm_libs
  ${AMDGPU_INSTR_BACKEND_LLVM_LINK_COMPONENTS}
)

add_library(amdgpu_instr_backend_llvm STATIC
  src/LlvmBackend.cpp
)

set_target_properties(amdgpu_instr_backend_llvm PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(amdgpu_instr_backend_llvm
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(amdgpu_instr_backend_llvm
  PUBLIC
    amdgpu_instr_backend
  PRIVATE
    ${amdgpu_instr_backend_llvm_libs}
)

target_compile_definitions(amdgpu_instr_backend_llvm
  PRIVATE
    ${LLVM_DEFINITIONS}
)

if(NEW_AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
