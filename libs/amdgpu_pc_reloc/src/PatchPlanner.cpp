//===-- PatchPlanner.cpp - Direct branch patches ----------------*- C++ -*-===//

#include "amdgpu_pc_reloc/PatchPlanner.h"
#include "amdgpu_pc_reloc/BranchMath.h"

namespace amdgpu_pc_reloc {

amdgpu_instr_backend::Result<SBranchPatchPlan>
PatchPlanner::planSBranchOverwrite(const SBranchPatchRequest &Req) const {
  using amdgpu_instr_backend::Result;

  if (Req.OverwriteSize < BranchMath::kInstructionBytes ||
      (Req.OverwriteSize % BranchMath::kInstructionBytes) != 0) {
    return Result<SBranchPatchPlan>::failure(
        "s_branch overwrite size must be a non-zero multiple of 4 bytes");
  }

  auto Dword = BranchMath::encodeDwordOffset(Req.PatchPC, Req.TargetPC);
  if (!Dword.Ok) {
    SBranchPatchPlan Plan;
    Plan.DwordOffset = Dword.DwordOffset;
    Plan.TargetPC = Req.TargetPC;
    Plan.Diagnostics.push_back(std::move(Dword.Diag));
    return Result<SBranchPatchPlan>::success(std::move(Plan));
  }

  auto Br = Backend.encodeSBranch(static_cast<int16_t>(Dword.DwordOffset));
  if (!Br) {
    return Result<SBranchPatchPlan>::failure(Br.error());
  }
  auto Nop = Backend.encodeNop();
  if (!Nop) {
    return Result<SBranchPatchPlan>::failure(Nop.error());
  }

  SBranchPatchPlan Plan;
  Plan.Bytes = Br.takeValue();
  auto NopBytes = Nop.takeValue();
  while (Plan.Bytes.size() < Req.OverwriteSize)
    Plan.Bytes.insert(Plan.Bytes.end(), NopBytes.begin(), NopBytes.end());
  Plan.DwordOffset = Dword.DwordOffset;
  Plan.TargetPC = Req.TargetPC;
  return Result<SBranchPatchPlan>::success(std::move(Plan));
}

} // namespace amdgpu_pc_reloc
