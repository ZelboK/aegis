//===-- QueueIntercept.cpp - HSA queue packet interception ------*- C++ -*-===//

#include "Internal.h"

#if defined(AEGIS_HAS_HSA_INTERCEPT)

#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <dlfcn.h>

#include <mutex>
#include <string>

namespace aegis::hsa_intercept::detail {

using HsaQueueCreateFn = hsa_status_t (*)(
    hsa_agent_t agent, uint32_t size, hsa_queue_type32_t type,
    void (*callback)(hsa_status_t status, hsa_queue_t* source, void* data),
    void* data, uint32_t privateSegmentSize, uint32_t groupSegmentSize,
    hsa_queue_t** queue);

using HsaInitFn = hsa_status_t (*)();
using HsaShutdownFn = hsa_status_t (*)();

using QueuePacketWriter = void (*)(const void* packets, uint64_t packetCount);

using QueueInterceptHandler =
    void (*)(const void* packets, uint64_t packetCount, uint64_t userPacketIndex,
             void* data, QueuePacketWriter writer);

using QueueInterceptRegisterFn =
    hsa_status_t (*)(hsa_queue_t* queue, QueueInterceptHandler callback,
                     void* userData);

using QueueInterceptCreateFn = hsa_status_t (*)(
    hsa_agent_t agent, uint32_t size, hsa_queue_type32_t type,
    void (*callback)(hsa_status_t status, hsa_queue_t* source, void* data),
    void* data, uint32_t privateSegmentSize, uint32_t groupSegmentSize,
    hsa_queue_t** queue);

namespace {

HsaQueueCreateFn realHsaQueueCreate() {
  static auto realFn =
      reinterpret_cast<HsaQueueCreateFn>(dlsym(RTLD_NEXT, "hsa_queue_create"));
  return realFn;
}

HsaInitFn realHsaInit() {
  static auto realFn =
      reinterpret_cast<HsaInitFn>(dlsym(RTLD_NEXT, "hsa_init"));
  return realFn;
}

HsaShutdownFn realHsaShutdown() {
  static auto realFn =
      reinterpret_cast<HsaShutdownFn>(dlsym(RTLD_NEXT, "hsa_shut_down"));
  return realFn;
}

void packetInterceptHandler(const void* packets, uint64_t packetCount,
                            uint64_t userPacketIndex, void* data,
                            QueuePacketWriter writer) {
  handlePacketWrite(packets, packetCount, userPacketIndex, data,
                    reinterpret_cast<void*>(writer));
}

} // namespace

Status registerQueueIntercept(QueueHandle queue) {
  if (!queue) {
    return makeStatus(StatusCode::invalidArgument, "Queue handle is null");
  }

  QueueInterceptRegisterFn registerFn = nullptr;
  {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    registerFn =
        reinterpret_cast<QueueInterceptRegisterFn>(s.queueInterceptRegisterFn);
  }

  if (!state().apiTableReady.load() || !registerFn) {
    return makeStatus(StatusCode::queueInterceptUnavailable,
                      "HSA queue intercept API is not available");
  }

  auto* hsaQueue = static_cast<hsa_queue_t*>(queue);
  hsa_status_t status =
      registerFn(hsaQueue, packetInterceptHandler, static_cast<void*>(hsaQueue));
  if (status != HSA_STATUS_SUCCESS) {
    return makeStatus(StatusCode::queueRegistrationFailed,
                      "Failed to register HSA queue interception");
  }

  {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.trackedQueues.push_back(queue);
  }
  return Status::success();
}

} // namespace aegis::hsa_intercept::detail

extern "C" {

hsa_status_t hsa_init() {
  auto realFn = aegis::hsa_intercept::detail::realHsaInit();
  if (!realFn) {
    return HSA_STATUS_ERROR;
  }

  hsa_status_t status = realFn();
  if (status == HSA_STATUS_SUCCESS &&
      !aegis::hsa_intercept::isInstalled()) {
    auto installStatus = aegis::hsa_intercept::install();
    if (!installStatus.ok()) {
      aegis::hsa_intercept::detail::log(installStatus.message());
    }
  }
  return status;
}

hsa_status_t hsa_queue_create(
    hsa_agent_t agent, uint32_t size, hsa_queue_type32_t type,
    void (*callback)(hsa_status_t status, hsa_queue_t* source, void* data),
    void* data, uint32_t privateSegmentSize, uint32_t groupSegmentSize,
    hsa_queue_t** queue) {
  auto realFn = aegis::hsa_intercept::detail::realHsaQueueCreate();
  if (!realFn) {
    return HSA_STATUS_ERROR;
  }

  auto& s = aegis::hsa_intercept::detail::state();
  aegis::hsa_intercept::detail::QueueInterceptCreateFn createFn = nullptr;
  aegis::hsa_intercept::detail::QueueInterceptRegisterFn registerFn = nullptr;
  bool enabled = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    enabled = s.options.enabled;
    createFn = reinterpret_cast<
        aegis::hsa_intercept::detail::QueueInterceptCreateFn>(
        s.queueInterceptCreateFn);
    registerFn = reinterpret_cast<
        aegis::hsa_intercept::detail::QueueInterceptRegisterFn>(
        s.queueInterceptRegisterFn);
  }

  if (!enabled || !s.installed.load() || !s.apiTableReady.load() || !createFn ||
      !registerFn) {
    return realFn(agent, size, type, callback, data, privateSegmentSize,
                  groupSegmentSize, queue);
  }

  hsa_status_t status =
      createFn(agent, size, type, callback, data, privateSegmentSize,
               groupSegmentSize, queue);
  if (status != HSA_STATUS_SUCCESS) {
    aegis::hsa_intercept::detail::log(
        "hsa_amd_queue_intercept_create failed; falling back to hsa_queue_create");
    return realFn(agent, size, type, callback, data, privateSegmentSize,
                  groupSegmentSize, queue);
  }

  status = registerFn(*queue, aegis::hsa_intercept::detail::packetInterceptHandler,
                      static_cast<void*>(*queue));
  if (status != HSA_STATUS_SUCCESS) {
    aegis::hsa_intercept::detail::log(
        "hsa_amd_queue_intercept_register failed");
  } else {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.trackedQueues.push_back(static_cast<void*>(*queue));
  }

  return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_shut_down() {
  auto realFn = aegis::hsa_intercept::detail::realHsaShutdown();
  if (!realFn) {
    return HSA_STATUS_ERROR;
  }

  aegis::hsa_intercept::uninstall();
  return realFn();
}

} // extern "C"

#endif // AEGIS_HAS_HSA_INTERCEPT
