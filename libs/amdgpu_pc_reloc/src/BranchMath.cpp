//===-- BranchMath.cpp - AMDGPU branch math ---------------------*- C++ -*-===//

#include "amdgpu_pc_reloc/BranchMath.h"

namespace amdgpu_pc_reloc {

BranchMath::DwordOffsetResult
BranchMath::encodeDwordOffset(uint64_t BranchPC, uint64_t Target) {
  if (!isDwordAligned(BranchPC) || !isDwordAligned(Target)) {
    return {false, 0,
            Diagnostic::error(
                DiagnosticCode::UnalignedTarget, BranchPC,
                "AMDGPU scalar branch PC and target must be 4-byte aligned")};
  }

  int64_t DeltaFromNext =
      static_cast<int64_t>(Target) -
      static_cast<int64_t>(BranchPC + kInstructionBytes);
  if ((DeltaFromNext % static_cast<int64_t>(kInstructionBytes)) != 0) {
    return {false, 0,
            Diagnostic::error(
                DiagnosticCode::UnalignedTarget, BranchPC,
                "AMDGPU scalar branch target is not dword reachable")};
  }

  int64_t DwordOffset = DeltaFromNext / static_cast<int64_t>(kInstructionBytes);
  if (!inSBranchRange(DwordOffset)) {
    return {false, DwordOffset,
            Diagnostic::error(DiagnosticCode::OutOfRangeBranch, BranchPC,
                              "AMDGPU scalar branch target is outside the "
                              "signed 16-bit dword range")};
  }

  return {true, DwordOffset, {}};
}

} // namespace amdgpu_pc_reloc
