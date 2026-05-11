//===-- Internal.h - HSA intercept private state ----------------*- C++ -*-===//

#ifndef AEGIS_HSA_INTERCEPT_INTERNAL_H
#define AEGIS_HSA_INTERCEPT_INTERNAL_H

#include "aegis/hsa_intercept/HsaIntercept.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegis::hsa_intercept::detail {

constexpr uint8_t kKernelDispatchPacketType = 2;

struct AqlKernelDispatchPacket {
  uint16_t header = 0;
  uint16_t setup = 0;
  uint16_t workgroupSizeX = 0;
  uint16_t workgroupSizeY = 0;
  uint16_t workgroupSizeZ = 0;
  uint16_t reserved0 = 0;
  uint32_t gridSizeX = 0;
  uint32_t gridSizeY = 0;
  uint32_t gridSizeZ = 0;
  uint32_t privateSegmentSize = 0;
  uint32_t groupSegmentSize = 0;
  uint64_t kernelObject = 0;
  uint64_t kernargAddress = 0;
  uint64_t reserved2 = 0;
  uint64_t completionSignal = 0;
};

static_assert(sizeof(AqlKernelDispatchPacket) == 64,
              "AQL kernel dispatch packets must remain 64 bytes");

using PacketWriter = void (*)(const void* packets, uint64_t packetCount);

struct State {
  std::atomic<bool> installed{false};
  std::atomic<bool> apiTableReady{false};

  Options options;
  Callbacks callbacks;
  Stats statistics;

  std::mutex mutex;
  std::vector<QueueHandle> trackedQueues;
  std::unordered_map<QueueHandle, uint64_t> queueAgents;
  std::unordered_map<uint64_t, CapturedCodeObject> codeObjects;

  void* queueInterceptCreateFn = nullptr;
  void* queueInterceptRegisterFn = nullptr;
  void* originalQueueCreateFn = nullptr;
  void* queueCreateSlot = nullptr;
};

State& state();

void log(std::string message);
Status makeStatus(StatusCode code, std::string message);
void debugLog(const char* location, const char* message,
              const char* hypothesisId, const std::string& data);

Status captureApiTable(void* tablePtr);
void captureExecutableState(uint64_t executableHandle);
Status registerQueueIntercept(QueueHandle queue);
void resetCapturedState();

} // namespace aegis::hsa_intercept::detail

#endif // AEGIS_HSA_INTERCEPT_INTERNAL_H
