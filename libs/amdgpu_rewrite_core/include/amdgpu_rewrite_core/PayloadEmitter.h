//===-- amdgpu_rewrite_core/PayloadEmitter.h --------------------*- C++ -*-===//

#ifndef AMDGPU_REWRITE_CORE_PAYLOAD_EMITTER_H
#define AMDGPU_REWRITE_CORE_PAYLOAD_EMITTER_H

#include "amdgpu_instr_backend/Backend.h"
#include "amdgpu_instr_backend/Result.h"
#include "amdgpu_rewrite_core/RewriteCore.h"
#include "amdgpu_rewrite_core/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amdgpu_rewrite_core {

struct PayloadEmissionRequest {
  InstrumentationLevel level = InstrumentationLevel::noopPatch;
  uint32_t siteId = 0;
  uint64_t entryPc = 0;
  uint64_t returnPc = 0;
  uint64_t profilingBufferAddress = 0;
  uint64_t profilingBufferSize = 0;
  unsigned tempVgprBaseIndex = 0;
  bool useAgprSpill = false;
  unsigned agprSpillBaseIndex = 0;
};

struct PayloadEmission {
  InstrumentationLevel level = InstrumentationLevel::noopPatch;
  uint32_t siteId = 0;
  uint64_t entryPc = 0;
  uint64_t returnPc = 0;
  std::string description;
  std::vector<uint8_t> bytes;
};

[[nodiscard]] amdgpu_instr_backend::Result<PayloadEmission>
emitPayload(const amdgpu_instr_backend::InstructionBackend &backend,
            const PayloadEmissionRequest &request);

} // namespace amdgpu_rewrite_core

#endif // AMDGPU_REWRITE_CORE_PAYLOAD_EMITTER_H
