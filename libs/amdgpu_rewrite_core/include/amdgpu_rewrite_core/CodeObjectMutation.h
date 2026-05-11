//===-- amdgpu_rewrite_core/CodeObjectMutation.h ----------------*- C++ -*-===//

#ifndef AMDGPU_REWRITE_CORE_CODE_OBJECT_MUTATION_H
#define AMDGPU_REWRITE_CORE_CODE_OBJECT_MUTATION_H

#include "amdgpu_instr_backend/Result.h"

#include <cstdint>
#include <vector>

namespace amdgpu_rewrite_core {

struct TextAppendRequest {
  std::vector<uint8_t> codeObjectBytes;
  uint64_t textBase = 0;
  uint64_t textOffset = 0;
  uint64_t textSize = 0;
  std::vector<uint8_t> bytesToAppend;
};

struct TextAppendResult {
  std::vector<uint8_t> codeObjectBytes;
  uint64_t appendedPc = 0;
  uint64_t textSize = 0;
};

[[nodiscard]] amdgpu_instr_backend::Result<TextAppendResult>
appendToText(const TextAppendRequest &request);

} // namespace amdgpu_rewrite_core

#endif // AMDGPU_REWRITE_CORE_CODE_OBJECT_MUTATION_H
