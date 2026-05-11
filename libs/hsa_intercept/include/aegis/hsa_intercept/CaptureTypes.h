//===-- aegis/hsa_intercept/CaptureTypes.h ----------------------*- C++ -*-===//

#ifndef AEGIS_HSA_INTERCEPT_CAPTURE_TYPES_H
#define AEGIS_HSA_INTERCEPT_CAPTURE_TYPES_H

#include "aegis/hsa_intercept/HsaTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegis::hsa_intercept {

/// Information captured when the HSA loader materializes a code object.
struct CapturedCodeObject {
  uint64_t codeObjectId = 0;
  std::vector<uint8_t> bytes;
  std::string uri;
  uint64_t loadBase = 0;
  uint64_t loadSize = 0;
};

/// Information captured for one kernel symbol in a loaded code object.
struct CapturedKernelSymbol {
  uint64_t kernelId = 0;
  uint64_t codeObjectId = 0;
  std::string kernelName;
  uint64_t kernelObject = 0;
  uint32_t kernargSegmentSize = 0;
  uint32_t groupSegmentSize = 0;
  uint32_t privateSegmentSize = 0;
};

/// Mutable view of the dispatch fields consumers are allowed to change.
struct DispatchPacket {
  uint16_t header = 0;
  uint64_t kernelObject = 0;
  uint64_t kernargAddress = 0;
  uint64_t completionSignal = 0;
};

/// Event emitted for a kernel dispatch packet observed at queue write time.
struct DispatchEvent {
  QueueHandle queue = nullptr;
  uint64_t queueAgent = 0;
  DispatchPacket packet;
  uint64_t originalKernelObject = 0;
  uint64_t originalKernargAddress = 0;
  uint32_t originalKernargSize = 0;
  uint64_t completionSignal = 0;
  uint64_t correlationId = 0;
};

} // namespace aegis::hsa_intercept

#endif // AEGIS_HSA_INTERCEPT_CAPTURE_TYPES_H
