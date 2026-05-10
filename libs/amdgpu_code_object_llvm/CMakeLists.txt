set(AMDGPU_CODE_OBJECT_LLVM_LINK_COMPONENTS
  BinaryFormat
  Object
  Support
)

llvm_map_components_to_libnames(amdgpu_code_object_llvm_libs
  ${AMDGPU_CODE_OBJECT_LLVM_LINK_COMPONENTS}
)

add_library(amdgpu_code_object_llvm STATIC
  src/LlvmCodeObjectParser.cpp
)

set_target_properties(amdgpu_code_object_llvm PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(amdgpu_code_object_llvm
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(amdgpu_code_object_llvm
  PUBLIC
    amdgpu_code_object
  PRIVATE
    ${amdgpu_code_object_llvm_libs}
)

target_compile_definitions(amdgpu_code_object_llvm
  PRIVATE
    ${LLVM_DEFINITIONS}
)

if(NEW_AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
