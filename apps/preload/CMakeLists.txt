add_library(aegis_preload SHARED
  PreloadRuntime.cpp
  $<TARGET_OBJECTS:aegis_hsa_intercept_objects>
)

target_include_directories(aegis_preload
  PRIVATE
    ${HSA_INCLUDE_DIR}
)

target_compile_definitions(aegis_preload
  PRIVATE
    AEGIS_HAS_HSA_INTERCEPT=1
)

if(NEW_AEGIS_HAS_LLVM_BACKEND)
  target_compile_definitions(aegis_preload
    PRIVATE
      NEW_AEGIS_HAS_LLVM_BACKEND=1
  )
endif()

target_link_libraries(aegis_preload
  PRIVATE
    aegisbit_runtime
    hsa_kernel_loader_hsa
    ${HSA_RUNTIME_LIBRARY}
    dl
)

if(NEW_AEGIS_HAS_LLVM_BACKEND)
  target_link_libraries(aegis_preload
    PRIVATE
      amdgpu_code_object_llvm
      amdgpu_instr_backend_llvm
  )
endif()

set_target_properties(aegis_preload PROPERTIES
  OUTPUT_NAME aegis
)
