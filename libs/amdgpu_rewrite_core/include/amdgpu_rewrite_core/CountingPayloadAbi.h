//===-- amdgpu_rewrite_core/CountingPayloadAbi.h ----------------*- C++ -*-===//

#ifndef AMDGPU_REWRITE_CORE_COUNTING_PAYLOAD_ABI_H
#define AMDGPU_REWRITE_CORE_COUNTING_PAYLOAD_ABI_H

#include <cstdint>

namespace amdgpu_rewrite_core {

struct CountingPayloadRecordV1 {
  static constexpr uint32_t magicValue = 0x41475331; // "AGS1"
  static constexpr uint32_t versionValue = 1;

  uint32_t magic = magicValue;
  uint32_t version = versionValue;
  uint64_t siteId = 0;
  uint64_t hitCount = 0;
};

static constexpr uint64_t countingPayloadRecordV1Size =
    sizeof(CountingPayloadRecordV1);

} // namespace amdgpu_rewrite_core

#endif // AMDGPU_REWRITE_CORE_COUNTING_PAYLOAD_ABI_H
