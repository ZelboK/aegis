//===-- amdgpu_pc_reloc/Relocator.h - AMDGPU PC relocator -------*- C++ -*-===//

#ifndef AMDGPU_PC_RELOC_RELOCATOR_H
#define AMDGPU_PC_RELOC_RELOCATOR_H

#include "amdgpu_pc_reloc/Types.h"
#include "amdgpu_instr_backend/Backend.h"
#include "amdgpu_instr_backend/ByteView.h"

#include <cstdint>
#include <vector>

namespace amdgpu_pc_reloc {

struct RelocateRangeRequest {
  amdgpu_instr_backend::ByteView Bytes;
  uint64_t SourceBase = 0;
  uint64_t DestinationBase = 0;
  std::vector<ByteRange> MovingRanges;
  std::vector<ByteRange> ProtectedRanges;
  std::vector<ByteRange> PatchedRanges;
  unsigned MaxGetPCGap = 16;
  bool ErrorOnUnmatchedGetPC = true;
};

struct RelocateRangeResult {
  std::vector<uint8_t> Bytes;
  std::vector<RelocationRecord> Records;
  std::vector<Diagnostic> Diagnostics;

  [[nodiscard]] bool hasErrors() const {
    for (const auto &D : Diagnostics)
      if (D.Severity == DiagnosticSeverity::Error)
        return true;
    return false;
  }
};

struct PcRelativeAddendRequest {
  std::vector<uint8_t> *Text = nullptr;
  uint64_t BodyStart = 0;
  uint64_t BodyEnd = 0;
  uint64_t Shift = 0;
  std::vector<ByteRange> PatchedRanges;
  unsigned MaxGetPCGap = 16;
};

struct PcRelativeAddendStats {
  uint32_t GetPCSeen = 0;
  uint32_t Matched = 0;
  uint32_t Rewritten = 0;
  uint32_t IntraBody = 0;
  uint32_t Skipped = 0;
  uint32_t EncodeFail = 0;
  std::vector<Diagnostic> Diagnostics;
};

class Relocator {
public:
  explicit Relocator(const amdgpu_instr_backend::InstructionBackend &Backend)
      : Backend(Backend) {}

  [[nodiscard]] amdgpu_instr_backend::Result<RelocateRangeResult>
  relocateRange(const RelocateRangeRequest &Req) const;

  [[nodiscard]] PcRelativeAddendStats
  rewritePCRelativeAddends(const PcRelativeAddendRequest &Req) const;

private:
  const amdgpu_instr_backend::InstructionBackend &Backend;
};

class RelocationSession {
public:
  RelocationSession(const amdgpu_instr_backend::InstructionBackend &Backend,
                    amdgpu_instr_backend::ByteView Bytes,
                    uint64_t BaseAddress);

  [[nodiscard]] amdgpu_instr_backend::Result<bool> decode();

  [[nodiscard]] amdgpu_instr_backend::Result<RelocateRangeResult>
  relocateCopy(uint64_t SourceStart, uint64_t Size, uint64_t DestinationStart,
               std::vector<ByteRange> ProtectedRanges = {},
               std::vector<ByteRange> PatchedRanges = {}) const;

  [[nodiscard]] const std::vector<amdgpu_instr_backend::Instruction> &
  instructions() const {
    return Instructions;
  }

private:
  const amdgpu_instr_backend::InstructionBackend &Backend;
  std::vector<uint8_t> Bytes;
  uint64_t BaseAddress = 0;
  std::vector<amdgpu_instr_backend::Instruction> Instructions;
};

} // namespace amdgpu_pc_reloc

#endif // AMDGPU_PC_RELOC_RELOCATOR_H
