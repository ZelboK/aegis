cmake_minimum_required(VERSION 3.20)

project(NewAegis VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

option(NEW_AEGIS_BUILD_TESTS "Build new_aegis tests" ON)
option(NEW_AEGIS_BUILD_PRELOAD "Build the preload shared library" ON)
option(NEW_AEGIS_ENABLE_LLVM_BACKEND "Build the LLVM AMDGPU instruction backend" ON)

set(ROCM_PATH "/opt/rocm" CACHE PATH "ROCm installation prefix")

find_path(HSA_INCLUDE_DIR
  NAMES hsa/hsa.h
  PATHS
    "${ROCM_PATH}/include"
    "/opt/rocm/include"
)

find_library(HSA_RUNTIME_LIBRARY
  NAMES hsa-runtime64
  PATHS
    "${ROCM_PATH}/lib"
    "/opt/rocm/lib"
)

if(HSA_INCLUDE_DIR AND HSA_RUNTIME_LIBRARY)
  set(AEGIS_HAS_HSA_INTERCEPT ON)
  message(STATUS "HSA runtime found; enabling hsa_intercept ROCm adapter")
else()
  set(AEGIS_HAS_HSA_INTERCEPT OFF)
  message(STATUS "HSA runtime not found; building hsa_intercept stubs only")
endif()

if(NEW_AEGIS_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()

if(NEW_AEGIS_ENABLE_LLVM_BACKEND)
  find_package(LLVM QUIET CONFIG
    PATHS
      "${CMAKE_CURRENT_SOURCE_DIR}/../../llvm-project-amd-staging/build/lib/cmake/llvm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../llvm-project/build/lib/cmake/llvm"
      "/usr/lib/llvm-23/lib/cmake/llvm"
    NO_DEFAULT_PATH
  )
  if(LLVM_FOUND)
    message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} found; enabling AMDGPU LLVM backend")
    set(NEW_AEGIS_HAS_LLVM_BACKEND ON)
  else()
    message(WARNING "LLVM not found; AMDGPU LLVM backend disabled")
    set(NEW_AEGIS_HAS_LLVM_BACKEND OFF)
  endif()
else()
  set(NEW_AEGIS_HAS_LLVM_BACKEND OFF)
endif()

add_subdirectory(libs/amdgpu_instr_backend)
add_subdirectory(libs/amdgpu_code_object)
add_subdirectory(libs/amdgpu_pc_reloc)
add_subdirectory(libs/amdgpu_rewrite_core)
add_subdirectory(libs/aegisbit_output)
add_subdirectory(libs/hsa_kernel_loader)
if(NEW_AEGIS_HAS_LLVM_BACKEND)
  add_subdirectory(libs/amdgpu_instr_backend_llvm)
  add_subdirectory(libs/amdgpu_code_object_llvm)
endif()
add_subdirectory(libs/hsa_intercept)
add_subdirectory(libs/aegisbit_runtime)

if(NEW_AEGIS_BUILD_PRELOAD AND AEGIS_HAS_HSA_INTERCEPT)
  add_subdirectory(apps/preload)
endif()
