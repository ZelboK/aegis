set(HSA_INTERCEPT_SOURCES
  src/HsaIntercept.cpp
)

if(AEGIS_HAS_HSA_INTERCEPT)
  list(APPEND HSA_INTERCEPT_SOURCES
    src/QueueIntercept.cpp
    src/ExecutableCapture.cpp
    src/RocprofilerApiTable.cpp
  )
endif()

add_library(aegis_hsa_intercept_objects OBJECT ${HSA_INTERCEPT_SOURCES})
set_target_properties(aegis_hsa_intercept_objects PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(aegis_hsa_intercept_objects
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(AEGIS_HAS_HSA_INTERCEPT)
  target_include_directories(aegis_hsa_intercept_objects
    PRIVATE
      ${HSA_INCLUDE_DIR}
  )
  target_compile_definitions(aegis_hsa_intercept_objects
    PRIVATE
      AEGIS_HAS_HSA_INTERCEPT=1
  )
endif()

add_library(aegis_hsa_intercept STATIC
  $<TARGET_OBJECTS:aegis_hsa_intercept_objects>
)

target_include_directories(aegis_hsa_intercept
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(AEGIS_HAS_HSA_INTERCEPT)
  target_include_directories(aegis_hsa_intercept
    PRIVATE
      ${HSA_INCLUDE_DIR}
  )
  target_compile_definitions(aegis_hsa_intercept
    PRIVATE
      AEGIS_HAS_HSA_INTERCEPT=1
  )
  target_link_libraries(aegis_hsa_intercept
    PRIVATE
      ${HSA_RUNTIME_LIBRARY}
      dl
  )
endif()

if(NEW_AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
