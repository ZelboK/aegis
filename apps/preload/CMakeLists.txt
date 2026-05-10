add_library(aegis_preload SHARED
  PreloadRuntime.cpp
  $<TARGET_OBJECTS:aegis_hsa_intercept_objects>
)

target_include_directories(aegis_preload
  PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/hsa_intercept/include
    ${HSA_INCLUDE_DIR}
)

target_compile_definitions(aegis_preload
  PRIVATE
    AEGIS_HAS_HSA_INTERCEPT=1
)

target_link_libraries(aegis_preload
  PRIVATE
    ${HSA_RUNTIME_LIBRARY}
    dl
)

set_target_properties(aegis_preload PROPERTIES
  OUTPUT_NAME aegis
)
