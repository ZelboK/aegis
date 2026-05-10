//===-- RocprofilerApiTable.cpp - HSA API table capture ---------*- C++ -*-===//

#include "Internal.h"

#if defined(AEGIS_HAS_HSA_INTERCEPT)

#include <dlfcn.h>

#include <cstdint>
#include <string>

namespace aegis::hsa_intercept::detail {

namespace {

struct ApiTableVersion {
  uint32_t major_id = 0;
  uint32_t minor_id = 0;
  uint32_t step_id = 0;
  uint32_t reserved = 0;
};

struct HsaApiTable {
  ApiTableVersion version;
  void* core_ = nullptr;
  void* amd_ext_ = nullptr;
  void* finalizer_ext_ = nullptr;
  void* image_ext_ = nullptr;
  void* tools_ = nullptr;
  void* pc_sampling_ext_ = nullptr;
};

constexpr size_t kQueueInterceptCreateIndex = 37;
constexpr size_t kQueueInterceptRegisterIndex = 38;
constexpr size_t kQueueCreateIndex = 8;

constexpr size_t kQueueInterceptCreateOffset =
    sizeof(ApiTableVersion) + (kQueueInterceptCreateIndex * sizeof(void*));
constexpr size_t kQueueInterceptRegisterOffset =
    sizeof(ApiTableVersion) + (kQueueInterceptRegisterIndex * sizeof(void*));
constexpr size_t kQueueCreateOffset =
    sizeof(ApiTableVersion) + (kQueueCreateIndex * sizeof(void*));

} // namespace

Status captureApiTable(void* tablePtr) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);

  if (!tablePtr) {
    return makeStatus(StatusCode::invalidArgument, "HSA API table is null");
  }

  auto* hsaTable = static_cast<HsaApiTable*>(tablePtr);
  if (!hsaTable->amd_ext_) {
    return makeStatus(StatusCode::apiTableUnavailable,
                      "HSA API table does not contain the AMD extension table");
  }

  auto* amdExtTable = static_cast<uint8_t*>(hsaTable->amd_ext_);
  s.queueInterceptCreateFn =
      *reinterpret_cast<void**>(amdExtTable + kQueueInterceptCreateOffset);
  s.queueInterceptRegisterFn =
      *reinterpret_cast<void**>(amdExtTable + kQueueInterceptRegisterOffset);

  if (hsaTable->core_) {
    auto* coreTable = static_cast<uint8_t*>(hsaTable->core_);
    s.queueCreateSlot = coreTable + kQueueCreateOffset;
    s.originalQueueCreateFn =
        *reinterpret_cast<void**>(coreTable + kQueueCreateOffset);
  }

  if (!s.queueInterceptCreateFn || !s.queueInterceptRegisterFn) {
    s.apiTableReady.store(false);
    return makeStatus(StatusCode::queueInterceptUnavailable,
                      "HSA AMD extension table lacks queue intercept functions");
  }

  s.apiTableReady.store(true);
  return Status::success();
}

} // namespace aegis::hsa_intercept::detail

extern "C" {

using rocprofiler_register_library_api_table_t =
    int (*)(const char*, void*, uint32_t, void**, uint64_t, void*);

int rocprofiler_register_library_api_table(const char* libName, void* importFunc,
                                           uint32_t libVersion,
                                           void** apiTables,
                                           uint64_t apiTableLength,
                                           void* registerId) {
  auto realFn = reinterpret_cast<rocprofiler_register_library_api_table_t>(
      dlsym(RTLD_NEXT, "rocprofiler_register_library_api_table"));

  bool shouldCapture = false;
  {
    auto& s = aegis::hsa_intercept::detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    shouldCapture = s.options.enableRocprofilerApiTableCapture;
  }

  if (shouldCapture && libName && apiTables && apiTableLength > 0 &&
      std::string(libName).find("hsa") != std::string::npos) {
    auto status = aegis::hsa_intercept::detail::captureApiTable(apiTables[0]);
    if (!status.ok()) {
      aegis::hsa_intercept::detail::log(status.message());
    }
  }

  if (!realFn) {
    return 1;
  }

  return realFn(libName, importFunc, libVersion, apiTables, apiTableLength,
                registerId);
}

} // extern "C"

#endif // AEGIS_HAS_HSA_INTERCEPT
