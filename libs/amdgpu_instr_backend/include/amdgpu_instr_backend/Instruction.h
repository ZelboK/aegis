//===-- amdgpu_instr_backend/Instruction.h ----------------------*- C++ -*-===//

#ifndef AMDGPU_INSTR_BACKEND_INSTRUCTION_H
#define AMDGPU_INSTR_BACKEND_INSTRUCTION_H

#include <cstdint>
#include <vector>

namespace amdgpu_instr_backend {

enum class OperandKind {
  other,
  registerValue,
  immediate,
};

struct Operand {
  OperandKind Kind = OperandKind::other;
  unsigned Register = 0;
  int64_t Immediate = 0;

  [[nodiscard]] static Operand reg(unsigned Value) {
    Operand Op;
    Op.Kind = OperandKind::registerValue;
    Op.Register = Value;
    return Op;
  }

  [[nodiscard]] static Operand imm(int64_t Value) {
    Operand Op;
    Op.Kind = OperandKind::immediate;
    Op.Immediate = Value;
    return Op;
  }

  [[nodiscard]] static Operand other() { return {}; }

  [[nodiscard]] bool isReg() const {
    return Kind == OperandKind::registerValue;
  }

  [[nodiscard]] bool isImm() const { return Kind == OperandKind::immediate; }
};

struct Instruction {
  unsigned BackendOpcode = 0;
  uint64_t Address = 0;
  uint64_t Size = 0;
  bool IsPCRelativeBranch = false;
  std::vector<Operand> Operands;
};

struct OpcodeInfo {
  unsigned SBranch = 0;
  unsigned SNop = 0;
  unsigned SGetPCB64 = 0;
  unsigned SAddU32 = 0;
  unsigned SAddcU32 = 0;
};

struct SgprPairInfo {
  unsigned PairReg = 0;
  unsigned LoReg = 0;
  unsigned HiReg = 0;
  unsigned LoIndex = 0;
};

} // namespace amdgpu_instr_backend

#endif // AMDGPU_INSTR_BACKEND_INSTRUCTION_H
