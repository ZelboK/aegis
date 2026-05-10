//===-- Relocator.cpp - AMDGPU PC relocator ---------------------*- C++ -*-===//

#include "amdgpu_pc_reloc/Relocator.h"
#include "amdgpu_pc_reloc/BranchMath.h"

#include <cstring>
#include <limits>
#include <utility>

namespace amdgpu_pc_reloc {
namespace {

using amdgpu_instr_backend::Instruction;
using amdgpu_instr_backend::InstructionBackend;
using amdgpu_instr_backend::Operand;
using amdgpu_instr_backend::Result;

bool extractConstant(const Operand &Op, int64_t &Out) {
  if (!Op.isImm()) {
    return false;
  }
  Out = Op.Immediate;
  return true;
}

int findFirstImmOperand(const Instruction &Inst) {
  for (size_t I = 0, E = Inst.Operands.size(); I < E; ++I) {
    int64_t Unused = 0;
    if (extractConstant(Inst.Operands[I], Unused))
      return static_cast<int>(I);
  }
  return -1;
}

unsigned regOperand(const Instruction &Inst, unsigned OpIdx) {
  if (OpIdx >= Inst.Operands.size()) {
    return 0;
  }
  const auto &Op = Inst.Operands[OpIdx];
  return Op.isReg() ? Op.Register : 0u;
}

bool addrInRanges(const std::vector<ByteRange> &Ranges, uint64_t Addr,
                  uint64_t Size) {
  return overlapsAny(Ranges, Addr, Addr + Size);
}

uint64_t translateTarget(uint64_t Target, const std::vector<ByteRange> &Moving,
                         int64_t Delta) {
  for (const auto &R : Moving)
    if (R.contains(Target))
      return static_cast<uint64_t>(static_cast<int64_t>(Target) + Delta);
  return Target;
}

bool tryReencodeSameSize(const InstructionBackend &Backend,
                         const Instruction &Orig, int ImmIdx, int32_t NewImm,
                         std::vector<uint8_t> &Out) {
  Instruction Mod = Orig;
  Mod.Operands[static_cast<size_t>(ImmIdx)] = Operand::imm(NewImm);
  auto BytesOrErr = Backend.encode(Mod);
  if (!BytesOrErr) {
    return false;
  }
  auto Bytes = BytesOrErr.takeValue();
  if (Bytes.size() != Orig.Size) {
    return false;
  }
  Out = std::move(Bytes);
  return true;
}

void appendError(std::vector<Diagnostic> &Diagnostics, DiagnosticCode Code,
                 uint64_t Address, const std::string &Message) {
  Diagnostics.push_back(Diagnostic::error(Code, Address, Message));
}

} // namespace

Result<RelocateRangeResult>
Relocator::relocateRange(const RelocateRangeRequest &Req) const {
  RelocateRangeResult Out;
  if (Req.Bytes.empty()) {
    return Result<RelocateRangeResult>::success(std::move(Out));
  }
  Out.Bytes.assign(Req.Bytes.data(), Req.Bytes.data() + Req.Bytes.size());

  if (overlapsAny(Req.ProtectedRanges, Req.DestinationBase,
                  Req.DestinationBase + Req.Bytes.size())) {
    Out.Diagnostics.push_back(Diagnostic::error(
        DiagnosticCode::ProtectedRangeOverlap, Req.DestinationBase,
        "relocated copy overlaps a protected destination range"));
    return Result<RelocateRangeResult>::success(std::move(Out));
  }

  auto InstsOrErr = Backend.decodeAll(Req.Bytes, Req.SourceBase);
  if (!InstsOrErr) {
    return Result<RelocateRangeResult>::failure(InstsOrErr.error());
  }
  const auto Insts = InstsOrErr.takeValue();

  std::vector<ByteRange> Moving = Req.MovingRanges;
  if (Moving.empty())
    Moving.push_back({Req.SourceBase, Req.SourceBase + Req.Bytes.size()});
  int64_t Delta = static_cast<int64_t>(Req.DestinationBase) -
                  static_cast<int64_t>(Req.SourceBase);

  for (const auto &DI : Insts) {
    if (!DI.IsPCRelativeBranch)
      continue;

    auto TargetOrErr = Backend.branchTarget(DI, DI.Address);
    if (!TargetOrErr) {
      return Result<RelocateRangeResult>::failure(TargetOrErr.error());
    }
    uint64_t Target = TargetOrErr.value();
    uint64_t NewPC = static_cast<uint64_t>(static_cast<int64_t>(DI.Address) +
                                           Delta);
    uint64_t NewTarget = translateTarget(Target, Moving, Delta);
    auto Dword = BranchMath::encodeDwordOffset(NewPC, NewTarget);
    if (!Dword.Ok) {
      Out.Diagnostics.push_back(std::move(Dword.Diag));
      continue;
    }

    Instruction Mod = DI;
    if (Mod.Operands.empty()) {
      appendError(Out.Diagnostics, DiagnosticCode::UnsupportedPCRelativeForm,
                  DI.Address, "PC-relative branch has no immediate operand");
      continue;
    }
    Mod.Operands.back() =
        Operand::imm(static_cast<int16_t>(Dword.DwordOffset));
    auto BytesOrErr = Backend.encode(Mod);
    if (!BytesOrErr) {
      return Result<RelocateRangeResult>::failure(BytesOrErr.error());
    }
    auto Bytes = BytesOrErr.takeValue();
    if (Bytes.size() != DI.Size) {
      appendError(Out.Diagnostics, DiagnosticCode::SizeChangingEncode,
                  DI.Address, "PC-relative branch re-encoded to a different "
                              "size during relocation");
      continue;
    }

    size_t Off = static_cast<size_t>(DI.Address - Req.SourceBase);
    std::memcpy(Out.Bytes.data() + Off, Bytes.data(), Bytes.size());
    Out.Records.push_back(
        {DI.Address, NewPC, Target, NewTarget, "pc-relative-branch"});
  }

  const auto &Opcodes = Backend.opcodes();
  for (size_t I = 0; I < Insts.size(); ++I) {
    const auto &GetPCI = Insts[I];
    if (GetPCI.BackendOpcode != Opcodes.SGetPCB64)
      continue;
    if (addrInRanges(Req.PatchedRanges, GetPCI.Address, GetPCI.Size))
      continue;

    if (GetPCI.Operands.empty() || !GetPCI.Operands[0].isReg()) {
      appendError(Out.Diagnostics, DiagnosticCode::UnsupportedPCRelativeForm,
                  GetPCI.Address, "s_getpc_b64 without a register-pair dest");
      continue;
    }
    const auto *Pair = Backend.getSgprPairInfo(GetPCI.Operands[0].Register);
    if (!Pair) {
      appendError(Out.Diagnostics, DiagnosticCode::UnsupportedPCRelativeForm,
                  GetPCI.Address, "unknown s_getpc_b64 destination pair");
      continue;
    }

    size_t AddAt = std::numeric_limits<size_t>::max();
    for (size_t J = I + 1; J < Insts.size() && J - I <= Req.MaxGetPCGap; ++J) {
      const auto &Candidate = Insts[J];
      if (addrInRanges(Req.PatchedRanges, Candidate.Address, Candidate.Size))
        continue;
      if (Candidate.BackendOpcode == Opcodes.SAddU32 &&
          regOperand(Candidate, 0) == Pair->LoReg &&
          regOperand(Candidate, 1) == Pair->LoReg &&
          findFirstImmOperand(Candidate) >= 0) {
        AddAt = J;
        break;
      }
      unsigned Dst = regOperand(Candidate, 0);
      if (Dst == Pair->LoReg || Dst == Pair->HiReg)
        break;
    }

    if (AddAt == std::numeric_limits<size_t>::max()) {
      if (Req.ErrorOnUnmatchedGetPC)
        appendError(Out.Diagnostics, DiagnosticCode::UnsupportedPCRelativeForm,
                    GetPCI.Address,
                    "s_getpc_b64 has no matching s_add_u32 immediate chain");
      continue;
    }

    size_t AddcAt = std::numeric_limits<size_t>::max();
    for (size_t J = AddAt + 1; J < Insts.size(); ++J) {
      const auto &Candidate = Insts[J];
      if (addrInRanges(Req.PatchedRanges, Candidate.Address, Candidate.Size))
        continue;
      if (Candidate.BackendOpcode == Opcodes.SAddcU32 &&
          regOperand(Candidate, 0) == Pair->HiReg &&
          regOperand(Candidate, 1) == Pair->HiReg &&
          findFirstImmOperand(Candidate) >= 0)
        AddcAt = J;
      break;
    }
    if (AddcAt == std::numeric_limits<size_t>::max()) {
      if (Req.ErrorOnUnmatchedGetPC)
        appendError(Out.Diagnostics, DiagnosticCode::UnsupportedPCRelativeForm,
                    GetPCI.Address,
                    "s_getpc_b64 add chain has no matching s_addc_u32");
      continue;
    }

    const auto &AddI = Insts[AddAt];
    const auto &AddcI = Insts[AddcAt];
    int AddImmIdx = findFirstImmOperand(AddI);
    int AddcImmIdx = findFirstImmOperand(AddcI);
    int64_t ImmLo = 0;
    int64_t ImmHi = 0;
    if (!extractConstant(AddI.Operands[static_cast<size_t>(AddImmIdx)],
                         ImmLo) ||
        !extractConstant(AddcI.Operands[static_cast<size_t>(AddcImmIdx)],
                         ImmHi)) {
      appendError(Out.Diagnostics, DiagnosticCode::UnsupportedPCRelativeForm,
                  GetPCI.Address, "s_getpc_b64 add chain immediates are not "
                                  "constant values");
      continue;
    }

    uint64_t AddendU =
        (static_cast<uint64_t>(static_cast<uint32_t>(ImmHi)) << 32) |
        static_cast<uint32_t>(ImmLo);
    int64_t AddendS = static_cast<int64_t>(AddendU);
    uint64_t TargetOrig = static_cast<uint64_t>(
        static_cast<int64_t>(GetPCI.Address + 4) + AddendS);
    uint64_t TargetNew = translateTarget(TargetOrig, Moving, Delta);
    uint64_t GetPCNew = static_cast<uint64_t>(
        static_cast<int64_t>(GetPCI.Address) + Delta);
    int64_t NewAddendS =
        static_cast<int64_t>(TargetNew) - static_cast<int64_t>(GetPCNew + 4);
    uint64_t NewAddendU = static_cast<uint64_t>(NewAddendS);
    int32_t NewImmLo = static_cast<int32_t>(NewAddendU & 0xFFFFFFFFu);
    int32_t NewImmHi =
        static_cast<int32_t>((NewAddendU >> 32) & 0xFFFFFFFFu);
    bool LoChanged =
        static_cast<uint32_t>(NewImmLo) != static_cast<uint32_t>(ImmLo);
    bool HiChanged =
        static_cast<uint32_t>(NewImmHi) != static_cast<uint32_t>(ImmHi);
    if (!LoChanged && !HiChanged)
      continue;

    std::vector<uint8_t> NewAddBytes;
    std::vector<uint8_t> NewAddcBytes;
    if (LoChanged &&
        !tryReencodeSameSize(Backend, AddI, AddImmIdx, NewImmLo,
                             NewAddBytes)) {
      appendError(Out.Diagnostics, DiagnosticCode::SizeChangingEncode,
                  AddI.Address, "s_add_u32 addend re-encoded to a different "
                                "size during relocation");
      continue;
    }
    if (HiChanged &&
        !tryReencodeSameSize(Backend, AddcI, AddcImmIdx, NewImmHi,
                             NewAddcBytes)) {
      appendError(Out.Diagnostics, DiagnosticCode::SizeChangingEncode,
                  AddcI.Address,
                  "s_addc_u32 addend re-encoded to a different size during "
                  "relocation");
      continue;
    }

    if (LoChanged) {
      size_t Off = static_cast<size_t>(AddI.Address - Req.SourceBase);
      std::memcpy(Out.Bytes.data() + Off, NewAddBytes.data(),
                  NewAddBytes.size());
    }
    if (HiChanged) {
      size_t Off = static_cast<size_t>(AddcI.Address - Req.SourceBase);
      std::memcpy(Out.Bytes.data() + Off, NewAddcBytes.data(),
                  NewAddcBytes.size());
    }
    Out.Records.push_back(
        {GetPCI.Address, GetPCNew, TargetOrig, TargetNew, "getpc-addend"});
  }

  return Result<RelocateRangeResult>::success(std::move(Out));
}

PcRelativeAddendStats
Relocator::rewritePCRelativeAddends(const PcRelativeAddendRequest &Req) const {
  PcRelativeAddendStats Stats;
  if (!Req.Text || Req.Shift == 0 || Req.BodyEnd <= Req.BodyStart)
    return Stats;

  const auto &Opcodes = Backend.opcodes();
  if (Opcodes.SGetPCB64 == 0 || Opcodes.SAddU32 == 0 ||
      Opcodes.SAddcU32 == 0) {
    Stats.Diagnostics.push_back(Diagnostic::warning(
        DiagnosticCode::UnsupportedPCRelativeForm, Req.BodyStart,
        "opcode discovery incomplete; PC-relative addend rewrite skipped"));
    return Stats;
  }

  amdgpu_instr_backend::ByteView Body(
      Req.Text->data() + Req.BodyStart,
      static_cast<size_t>(Req.BodyEnd - Req.BodyStart));
  auto InstsOrErr = Backend.decodeAll(Body, Req.BodyStart);
  if (!InstsOrErr) {
    Stats.Diagnostics.push_back(Diagnostic::error(
        DiagnosticCode::DecodeFailed, Req.BodyStart, InstsOrErr.error()));
    return Stats;
  }
  const auto Insts = InstsOrErr.takeValue();
  uint64_t OrigBodyStart = Req.BodyStart - Req.Shift;
  uint64_t OrigBodyEnd = Req.BodyEnd - Req.Shift;

  for (size_t I = 0; I < Insts.size(); ++I) {
    const auto &GetPCI = Insts[I];
    if (GetPCI.BackendOpcode != Opcodes.SGetPCB64)
      continue;
    if (addrInRanges(Req.PatchedRanges, GetPCI.Address, GetPCI.Size))
      continue;
    Stats.GetPCSeen++;

    if (GetPCI.Operands.empty() || !GetPCI.Operands[0].isReg()) {
      Stats.Skipped++;
      continue;
    }
    const auto *Pair = Backend.getSgprPairInfo(GetPCI.Operands[0].Register);
    if (!Pair) {
      Stats.Skipped++;
      continue;
    }

    size_t AddAt = std::numeric_limits<size_t>::max();
    for (size_t J = I + 1; J < Insts.size() && J - I <= Req.MaxGetPCGap; ++J) {
      const auto &Candidate = Insts[J];
      if (addrInRanges(Req.PatchedRanges, Candidate.Address, Candidate.Size))
        continue;
      if (Candidate.BackendOpcode == Opcodes.SAddU32 &&
          regOperand(Candidate, 0) == Pair->LoReg &&
          regOperand(Candidate, 1) == Pair->LoReg &&
          findFirstImmOperand(Candidate) >= 0) {
        AddAt = J;
        break;
      }
      unsigned Dst = regOperand(Candidate, 0);
      if (Dst == Pair->LoReg || Dst == Pair->HiReg)
        break;
    }
    if (AddAt == std::numeric_limits<size_t>::max()) {
      Stats.Skipped++;
      continue;
    }

    size_t AddcAt = std::numeric_limits<size_t>::max();
    for (size_t J = AddAt + 1; J < Insts.size(); ++J) {
      const auto &Candidate = Insts[J];
      if (addrInRanges(Req.PatchedRanges, Candidate.Address, Candidate.Size))
        continue;
      if (Candidate.BackendOpcode == Opcodes.SAddcU32 &&
          regOperand(Candidate, 0) == Pair->HiReg &&
          regOperand(Candidate, 1) == Pair->HiReg &&
          findFirstImmOperand(Candidate) >= 0)
        AddcAt = J;
      break;
    }
    if (AddcAt == std::numeric_limits<size_t>::max()) {
      Stats.Skipped++;
      continue;
    }
    Stats.Matched++;

    const auto &AddI = Insts[AddAt];
    const auto &AddcI = Insts[AddcAt];
    int AddImmIdx = findFirstImmOperand(AddI);
    int AddcImmIdx = findFirstImmOperand(AddcI);
    int64_t ImmLo = 0;
    int64_t ImmHi = 0;
    if (!extractConstant(AddI.Operands[static_cast<size_t>(AddImmIdx)],
                         ImmLo) ||
        !extractConstant(AddcI.Operands[static_cast<size_t>(AddcImmIdx)],
                         ImmHi)) {
      Stats.EncodeFail++;
      continue;
    }

    uint64_t AddendU =
        (static_cast<uint64_t>(static_cast<uint32_t>(ImmHi)) << 32) |
        static_cast<uint32_t>(ImmLo);
    int64_t AddendS = static_cast<int64_t>(AddendU);
    int64_t TargetOrig =
        static_cast<int64_t>(GetPCI.Address - Req.Shift + 4) + AddendS;

    if (TargetOrig >= static_cast<int64_t>(OrigBodyStart) &&
        TargetOrig < static_cast<int64_t>(OrigBodyEnd)) {
      Stats.IntraBody++;
      continue;
    }

    int64_t NewAddendS = AddendS - static_cast<int64_t>(Req.Shift);
    uint64_t NewAddendU = static_cast<uint64_t>(NewAddendS);
    int32_t NewImmLo = static_cast<int32_t>(NewAddendU & 0xFFFFFFFFu);
    int32_t NewImmHi =
        static_cast<int32_t>((NewAddendU >> 32) & 0xFFFFFFFFu);
    bool LoChanged =
        static_cast<uint32_t>(NewImmLo) != static_cast<uint32_t>(ImmLo);
    bool HiChanged =
        static_cast<uint32_t>(NewImmHi) != static_cast<uint32_t>(ImmHi);
    if (!LoChanged && !HiChanged) {
      Stats.IntraBody++;
      continue;
    }

    std::vector<uint8_t> NewAddBytes;
    std::vector<uint8_t> NewAddcBytes;
    if (LoChanged &&
        !tryReencodeSameSize(Backend, AddI, AddImmIdx, NewImmLo,
                             NewAddBytes)) {
      Stats.EncodeFail++;
      continue;
    }
    if (HiChanged &&
        !tryReencodeSameSize(Backend, AddcI, AddcImmIdx, NewImmHi,
                             NewAddcBytes)) {
      Stats.EncodeFail++;
      continue;
    }

    if (LoChanged)
      std::memcpy(Req.Text->data() + AddI.Address, NewAddBytes.data(),
                  NewAddBytes.size());
    if (HiChanged)
      std::memcpy(Req.Text->data() + AddcI.Address, NewAddcBytes.data(),
                  NewAddcBytes.size());
    Stats.Rewritten++;
  }

  return Stats;
}

RelocationSession::RelocationSession(const InstructionBackend &Backend,
                                     amdgpu_instr_backend::ByteView Bytes,
                                     uint64_t BaseAddress)
    : Backend(Backend), BaseAddress(BaseAddress) {
  if (!Bytes.empty()) {
    this->Bytes.assign(Bytes.data(), Bytes.data() + Bytes.size());
  }
}

Result<bool> RelocationSession::decode() {
  auto InstsOrErr = Backend.decodeAll(Bytes, BaseAddress);
  if (!InstsOrErr) {
    return Result<bool>::failure(InstsOrErr.error());
  }
  Instructions = InstsOrErr.takeValue();
  return Result<bool>::success(true);
}

Result<RelocateRangeResult> RelocationSession::relocateCopy(
    uint64_t SourceStart, uint64_t Size, uint64_t DestinationStart,
    std::vector<ByteRange> ProtectedRanges,
    std::vector<ByteRange> PatchedRanges) const {
  if (SourceStart < BaseAddress ||
      SourceStart + Size > BaseAddress + Bytes.size()) {
    return Result<RelocateRangeResult>::failure(
        "relocateCopy source range is outside the session text range");
  }
  Relocator R(Backend);
  size_t Off = static_cast<size_t>(SourceStart - BaseAddress);
  RelocateRangeRequest Req;
  Req.Bytes = amdgpu_instr_backend::ByteView(Bytes.data() + Off,
                                             static_cast<size_t>(Size));
  Req.SourceBase = SourceStart;
  Req.DestinationBase = DestinationStart;
  Req.MovingRanges = {{SourceStart, SourceStart + Size}};
  Req.ProtectedRanges = std::move(ProtectedRanges);
  Req.PatchedRanges = std::move(PatchedRanges);
  return R.relocateRange(Req);
}

} // namespace amdgpu_pc_reloc
