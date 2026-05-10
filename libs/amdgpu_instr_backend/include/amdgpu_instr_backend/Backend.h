//===-- amdgpu_instr_backend/Backend.h --------------------------*- C++ -*-===//

#ifndef AMDGPU_INSTR_BACKEND_BACKEND_H
#define AMDGPU_INSTR_BACKEND_BACKEND_H

#include "amdgpu_instr_backend/ByteView.h"
#include "amdgpu_instr_backend/Instruction.h"
#include "amdgpu_instr_backend/Result.h"

#include <cstdint>
#include <vector>

namespace amdgpu_instr_backend {

class InstructionBackend {
public:
  virtual ~InstructionBackend() = default;

  [[nodiscard]] virtual const OpcodeInfo &opcodes() const = 0;

  [[nodiscard]] virtual Result<Instruction> decode(ByteView Bytes,
                                                   uint64_t Address) const = 0;

  [[nodiscard]] virtual Result<std::vector<Instruction>>
  decodeAll(ByteView Bytes, uint64_t BaseAddress) const = 0;

  [[nodiscard]] virtual Result<std::vector<uint8_t>>
  encode(const Instruction &Inst) const = 0;

  [[nodiscard]] virtual Result<std::vector<uint8_t>>
  encodeSBranch(int16_t DwordOffset) const = 0;

  [[nodiscard]] virtual Result<std::vector<uint8_t>> encodeNop() const = 0;

  [[nodiscard]] virtual Result<uint64_t>
  branchTarget(const Instruction &Inst, uint64_t CurrentPC) const = 0;

  [[nodiscard]] virtual const SgprPairInfo *
  getSgprPairInfo(unsigned PairReg) const = 0;
};

} // namespace amdgpu_instr_backend

#endif // AMDGPU_INSTR_BACKEND_BACKEND_H
