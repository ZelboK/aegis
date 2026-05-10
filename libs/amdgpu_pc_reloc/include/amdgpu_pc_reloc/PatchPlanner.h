//===-- amdgpu_pc_reloc/PatchPlanner.h - Direct branch patches ---*- C++ -*-===//

#ifndef AMDGPU_PC_RELOC_PATCH_PLANNER_H
#define AMDGPU_PC_RELOC_PATCH_PLANNER_H

#include "amdgpu_pc_reloc/Types.h"
#include "amdgpu_instr_backend/Backend.h"

#include <cstdint>
#include <vector>

namespace amdgpu_pc_reloc {

struct SBranchPatchRequest {
  uint64_t PatchPC = 0;
  uint64_t TargetPC = 0;
  uint64_t OverwriteSize = 4;
};

struct SBranchPatchPlan {
  std::vector<uint8_t> Bytes;
  int64_t DwordOffset = 0;
  uint64_t TargetPC = 0;
  std::vector<Diagnostic> Diagnostics;
};

class PatchPlanner {
public:
  explicit PatchPlanner(
      const amdgpu_instr_backend::InstructionBackend &Backend)
      : Backend(Backend) {}

  [[nodiscard]] amdgpu_instr_backend::Result<SBranchPatchPlan>
  planSBranchOverwrite(const SBranchPatchRequest &Req) const;

private:
  const amdgpu_instr_backend::InstructionBackend &Backend;
};

} // namespace amdgpu_pc_reloc

#endif // AMDGPU_PC_RELOC_PATCH_PLANNER_H
