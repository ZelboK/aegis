//===-- amdgpu_pc_reloc/BranchMath.h - AMDGPU branch math -------*- C++ -*-===//

#ifndef AMDGPU_PC_RELOC_BRANCH_MATH_H
#define AMDGPU_PC_RELOC_BRANCH_MATH_H

#include "amdgpu_pc_reloc/Types.h"

#include <cstdint>

namespace amdgpu_pc_reloc {

class BranchMath {
public:
  static constexpr uint64_t kInstructionBytes = 4;
  static constexpr int64_t kSBranchMinDword = -32768;
  static constexpr int64_t kSBranchMaxDword = 32767;

  struct DwordOffsetResult {
    bool Ok = false;
    int64_t DwordOffset = 0;
    Diagnostic Diag;
  };

  [[nodiscard]] static bool isDwordAligned(uint64_t Address) {
    return (Address % kInstructionBytes) == 0;
  }

  [[nodiscard]] static bool inSBranchRange(int64_t DwordOffset) {
    return DwordOffset >= kSBranchMinDword &&
           DwordOffset <= kSBranchMaxDword;
  }

  [[nodiscard]] static int64_t byteDistance(uint64_t FromPC,
                                            uint64_t Target) {
    return static_cast<int64_t>(Target) - static_cast<int64_t>(FromPC);
  }

  [[nodiscard]] static uint64_t branchTarget(uint64_t BranchPC,
                                             int16_t DwordOffset) {
    return static_cast<uint64_t>(
        static_cast<int64_t>(BranchPC + kInstructionBytes) +
        static_cast<int64_t>(DwordOffset) * kInstructionBytes);
  }

  [[nodiscard]] static DwordOffsetResult encodeDwordOffset(uint64_t BranchPC,
                                                           uint64_t Target);
};

} // namespace amdgpu_pc_reloc

#endif // AMDGPU_PC_RELOC_BRANCH_MATH_H
