//===-- PayloadEmitter.cpp - Minimal payload byte emission ----------------===//

#include "amdgpu_rewrite_core/PayloadEmitter.h"

#include <utility>

namespace amdgpu_rewrite_core {
namespace {

using amdgpu_instr_backend::Result;

void append(std::vector<uint8_t> &out, std::vector<uint8_t> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

} // namespace

Result<PayloadEmission>
emitPayload(const amdgpu_instr_backend::InstructionBackend &backend,
            const PayloadEmissionRequest &request) {
  PayloadEmission emission;
  emission.level = request.level;
  emission.siteId = request.siteId;
  emission.entryPc = request.entryPc;
  emission.returnPc = request.returnPc;

  if (request.level == InstrumentationLevel::noopPatch) {
    emission.description = "noopatch emits no payload bytes";
    return Result<PayloadEmission>::success(std::move(emission));
  }

  if (request.level == InstrumentationLevel::dryPayload) {
    auto nop = backend.encodeNop();
    if (!nop) {
      return Result<PayloadEmission>::failure(nop.error());
    }
    append(emission.bytes, nop.value());
    emission.description = "DryPayload control-flow detour";
  } else if (request.level == InstrumentationLevel::countingPayload) {
    if (request.profilingBufferAddress == 0 || request.profilingBufferSize == 0) {
      return Result<PayloadEmission>::failure(
          "counting payload requires a profiling buffer");
    }
    auto recordWrite = backend.encodeCountingRecordWrite(
        {request.profilingBufferAddress, request.profilingBufferSize,
         request.siteId, 1, request.tempVgprBaseIndex,
         request.tempVgprBaseIndex + 1, request.tempVgprBaseIndex + 2,
         request.useAgprSpill, request.agprSpillBaseIndex});
    if (!recordWrite) {
      return Result<PayloadEmission>::failure(recordWrite.error());
    }
    append(emission.bytes, recordWrite.value());
    emission.description = "CountingPayload control-flow detour";
  } else {
    return Result<PayloadEmission>::failure("unknown instrumentation level");
  }

  return Result<PayloadEmission>::success(std::move(emission));
}

} // namespace amdgpu_rewrite_core
