//===-- LlvmBackend.cpp - LLVM AMDGPU instruction backend -------*- C++ -*-===//

#include "amdgpu_instr_backend_llvm/LlvmBackend.h"

#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <sstream>
#include <unordered_map>
#include <utility>

extern "C" {
void LLVMInitializeAMDGPUTargetInfo();
void LLVMInitializeAMDGPUTarget();
void LLVMInitializeAMDGPUTargetMC();
void LLVMInitializeAMDGPUDisassembler();
}

namespace amdgpu_instr_backend_llvm {
namespace {

using amdgpu_instr_backend::ByteView;
using amdgpu_instr_backend::Instruction;
using amdgpu_instr_backend::InstructionBackend;
using amdgpu_instr_backend::OpcodeInfo;
using amdgpu_instr_backend::Operand;
using amdgpu_instr_backend::Result;
using amdgpu_instr_backend::SgprPairInfo;

template <typename T> Result<T> failureFromError(llvm::Error Err) {
  return Result<T>::failure(llvm::toString(std::move(Err)));
}

bool extractConstant(const llvm::MCOperand &Op, int64_t &Out) {
  if (Op.isImm()) {
    Out = Op.getImm();
    return true;
  }
  if (Op.isExpr()) {
    const llvm::MCExpr *Expr = Op.getExpr();
    return Expr && Expr->evaluateAsAbsolute(Out);
  }
  return false;
}

class LlvmBackend final : public InstructionBackend {
public:
  LlvmBackend(std::unique_ptr<llvm::MCContext> Ctx,
              std::unique_ptr<llvm::MCDisassembler> Disasm,
              std::unique_ptr<llvm::MCCodeEmitter> Emitter,
              std::unique_ptr<llvm::MCInstrInfo> MCII,
              std::unique_ptr<llvm::MCRegisterInfo> MRI,
              std::unique_ptr<llvm::MCSubtargetInfo> STI,
              std::unique_ptr<llvm::MCAsmInfo> MAI,
              std::unique_ptr<llvm::MCInstPrinter> Printer,
              const llvm::Target *TheTarget)
      : Ctx(std::move(Ctx)), DisasmImpl(std::move(Disasm)),
        Emitter(std::move(Emitter)), MCII(std::move(MCII)),
        MRI(std::move(MRI)), STI(std::move(STI)), MAI(std::move(MAI)),
        Printer(std::move(Printer)), TheTarget(TheTarget) {}

  const OpcodeInfo &opcodes() const override { return Opcodes; }

  Result<Instruction> decode(ByteView Bytes, uint64_t Address) const override {
    llvm::MCInst Inst;
    uint64_t Size = 0;
    llvm::MCDisassembler::DecodeStatus Status = DisasmImpl->getInstruction(
        Inst, Size, llvm::ArrayRef<uint8_t>(Bytes.data(), Bytes.size()),
        Address, llvm::nulls());
    if (Status != llvm::MCDisassembler::Success) {
      std::ostringstream OS;
      OS << "failed to disassemble AMDGPU instruction at 0x" << std::hex
         << Address;
      return Result<Instruction>::failure(
          OS.str());
    }
    return Result<Instruction>::success(convert(Inst, Address, Size));
  }

  Result<std::vector<Instruction>>
  decodeAll(ByteView Bytes, uint64_t BaseAddress) const override {
    std::vector<Instruction> Instructions;
    size_t Offset = 0;
    while (Offset < Bytes.size()) {
      auto DI = decode(Bytes.slice(Offset), BaseAddress + Offset);
      if (!DI) {
        return Result<std::vector<Instruction>>::failure(DI.error());
      }
      auto Inst = DI.takeValue();
      if (Inst.Size == 0) {
        return Result<std::vector<Instruction>>::failure(
            "decoder returned a zero-size instruction");
      }
      Offset += static_cast<size_t>(Inst.Size);
      Instructions.push_back(std::move(Inst));
    }
    return Result<std::vector<Instruction>>::success(std::move(Instructions));
  }

  Result<std::vector<uint8_t>> encode(const Instruction &Inst) const override {
    llvm::MCInst MI;
    MI.setOpcode(Inst.BackendOpcode);
    for (const auto &Op : Inst.Operands) {
      if (Op.isReg()) {
        MI.addOperand(llvm::MCOperand::createReg(Op.Register));
      } else if (Op.isImm()) {
        MI.addOperand(llvm::MCOperand::createImm(Op.Immediate));
      } else {
        return Result<std::vector<uint8_t>>::failure(
            "cannot encode instruction with non-register/non-immediate "
            "operand");
      }
    }
    return encodeMCInst(MI);
  }

  Result<std::vector<uint8_t>> encodeSBranch(int16_t DwordOffset) const override {
    llvm::MCInst MI;
    MI.setOpcode(Opcodes.SBranch);
    MI.addOperand(llvm::MCOperand::createImm(DwordOffset));
    return encodeMCInst(MI);
  }

  Result<std::vector<uint8_t>> encodeNop() const override {
    llvm::MCInst MI;
    MI.setOpcode(Opcodes.SNop);
    MI.addOperand(llvm::MCOperand::createImm(0));
    return encodeMCInst(MI);
  }

  Result<uint64_t> branchTarget(const Instruction &Inst,
                                uint64_t CurrentPC) const override {
    if (!Inst.IsPCRelativeBranch || Inst.Operands.empty() ||
        !Inst.Operands.back().isImm()) {
      return Result<uint64_t>::failure(
          "instruction is not a PC-relative branch");
    }
    int16_t DwordOffset = static_cast<int16_t>(Inst.Operands.back().Immediate);
    uint64_t Target = static_cast<uint64_t>(
        static_cast<int64_t>(CurrentPC + 4) +
        static_cast<int64_t>(DwordOffset) * 4);
    return Result<uint64_t>::success(Target);
  }

  const SgprPairInfo *getSgprPairInfo(unsigned PairReg) const override {
    auto It = PairInfoByReg.find(PairReg);
    return It == PairInfoByReg.end() ? nullptr : &It->second;
  }

  llvm::Error discoverOpcodesAndRegisters() {
    {
      uint8_t B[] = {0x00, 0x00, 0x82, 0xBF}; // s_branch 0
      if (auto E = tryDecodeOpcode(B, Opcodes.SBranch))
        return E;
    }
    {
      uint8_t B[] = {0x00, 0x00, 0x80, 0xBF}; // s_nop 0
      if (auto E = tryDecodeOpcode(B, Opcodes.SNop))
        return E;
    }
    {
      uint8_t B[] = {0x00, 0x1C, 0x80, 0xBE}; // s_getpc_b64 s[0:1]
      if (auto E = tryDecodeOpcode(B, Opcodes.SGetPCB64))
        return E;
    }
    {
      uint8_t B[] = {0x00, 0x80, 0x00, 0x80}; // s_add_u32 s0, s0, 0
      if (auto E = tryDecodeOpcode(B, Opcodes.SAddU32))
        return E;
    }
    {
      uint8_t B[] = {0x00, 0x80, 0x00, 0x82}; // s_addc_u32 s0, s0, 0
      if (auto E = tryDecodeOpcode(B, Opcodes.SAddcU32))
        return E;
    }

    for (unsigned Index = 0; Index + 1 < 106; Index += 2) {
      auto InfoOrErr = resolveSgprPair(Index);
      if (!InfoOrErr) {
        continue;
      }
      auto Info = InfoOrErr.takeValue();
      PairInfoByReg[Info.PairReg] = Info;
    }
    return llvm::Error::success();
  }

private:
  Result<std::vector<uint8_t>> encodeMCInst(const llvm::MCInst &Inst) const {
    llvm::SmallVector<char, 16> Code;
    llvm::SmallVector<llvm::MCFixup, 4> Fixups;
    Emitter->encodeInstruction(Inst, Code, Fixups, *STI);
    if (!Fixups.empty()) {
      return Result<std::vector<uint8_t>>::failure(
          "instruction encoding produced fixups; symbolic relocations are not "
          "supported by amdgpu_instr_backend_llvm");
    }

    std::vector<uint8_t> Bytes;
    Bytes.reserve(Code.size());
    for (char C : Code) {
      Bytes.push_back(static_cast<uint8_t>(C));
    }
    return Result<std::vector<uint8_t>>::success(std::move(Bytes));
  }

  Instruction convert(const llvm::MCInst &Inst, uint64_t Address,
                      uint64_t Size) const {
    Instruction Out;
    Out.BackendOpcode = Inst.getOpcode();
    Out.Address = Address;
    Out.Size = Size;
    const llvm::MCInstrDesc &Desc = MCII->get(Inst.getOpcode());
    Out.IsPCRelativeBranch =
        Desc.isBranch() && !Desc.isReturn() && !Desc.isCall() &&
        Inst.getNumOperands() > 0 &&
        Inst.getOperand(Inst.getNumOperands() - 1).isImm();
    Out.Operands.reserve(Inst.getNumOperands());
    for (unsigned I = 0, E = Inst.getNumOperands(); I < E; ++I) {
      const auto &Op = Inst.getOperand(I);
      if (Op.isReg()) {
        Out.Operands.push_back(Operand::reg(Op.getReg()));
        continue;
      }
      int64_t Imm = 0;
      if (extractConstant(Op, Imm)) {
        Out.Operands.push_back(Operand::imm(Imm));
        continue;
      }
      Out.Operands.push_back(Operand::other());
    }
    return Out;
  }

  llvm::Error tryDecodeOpcode(ByteView Bytes, unsigned &Out) const {
    auto DI = decode(Bytes, 0);
    if (!DI) {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     DI.error());
    }
    Out = DI.value().BackendOpcode;
    return llvm::Error::success();
  }

  Result<unsigned> resolveSgprReg(unsigned Index) const {
    uint32_t Encoding = 0x80008000u | (Index << 16) | Index;
    uint8_t Bytes[4] = {static_cast<uint8_t>(Encoding & 0xFF),
                        static_cast<uint8_t>((Encoding >> 8) & 0xFF),
                        static_cast<uint8_t>((Encoding >> 16) & 0xFF),
                        static_cast<uint8_t>((Encoding >> 24) & 0xFF)};
    auto DI = decode(Bytes, 0);
    if (!DI) {
      return Result<unsigned>::failure(DI.error());
    }
    const auto &Inst = DI.value();
    if (Inst.Operands.empty() || !Inst.Operands[0].isReg()) {
      return Result<unsigned>::failure("failed to resolve SGPR register");
    }
    return Result<unsigned>::success(Inst.Operands[0].Register);
  }

  Result<SgprPairInfo> resolveSgprPair(unsigned EvenIndex) const {
    uint32_t Encoding = 0xBA800000u | (EvenIndex << 16);
    uint8_t Bytes[4] = {static_cast<uint8_t>(Encoding & 0xFF),
                        static_cast<uint8_t>((Encoding >> 8) & 0xFF),
                        static_cast<uint8_t>((Encoding >> 16) & 0xFF),
                        static_cast<uint8_t>((Encoding >> 24) & 0xFF)};
    auto DI = decode(Bytes, 0);
    if (!DI) {
      return Result<SgprPairInfo>::failure(DI.error());
    }
    const auto &Inst = DI.value();
    if (Inst.Operands.empty() || !Inst.Operands[0].isReg()) {
      return Result<SgprPairInfo>::failure(
          "failed to resolve SGPR pair register");
    }

    auto Lo = resolveSgprReg(EvenIndex);
    if (!Lo) {
      return Result<SgprPairInfo>::failure(Lo.error());
    }
    auto Hi = resolveSgprReg(EvenIndex + 1);
    if (!Hi) {
      return Result<SgprPairInfo>::failure(Hi.error());
    }

    return Result<SgprPairInfo>::success(
        {Inst.Operands[0].Register, Lo.value(), Hi.value(), EvenIndex});
  }

  std::unique_ptr<llvm::MCContext> Ctx;
  std::unique_ptr<llvm::MCDisassembler> DisasmImpl;
  std::unique_ptr<llvm::MCCodeEmitter> Emitter;
  std::unique_ptr<llvm::MCInstrInfo> MCII;
  std::unique_ptr<llvm::MCRegisterInfo> MRI;
  std::unique_ptr<llvm::MCSubtargetInfo> STI;
  std::unique_ptr<llvm::MCAsmInfo> MAI;
  std::unique_ptr<llvm::MCInstPrinter> Printer;
  const llvm::Target *TheTarget = nullptr;
  OpcodeInfo Opcodes;
  std::unordered_map<unsigned, SgprPairInfo> PairInfoByReg;
};

} // namespace

Result<std::unique_ptr<InstructionBackend>>
createLlvmBackend(LlvmBackendOptions Options) {
  LLVMInitializeAMDGPUTargetInfo();
  LLVMInitializeAMDGPUTarget();
  LLVMInitializeAMDGPUTargetMC();
  LLVMInitializeAMDGPUDisassembler();

  llvm::Triple TT(Options.TargetTriple);
  std::string Error;
  const llvm::Target *TheTarget = llvm::TargetRegistry::lookupTarget(TT, Error);
  if (!TheTarget) {
    return Result<std::unique_ptr<InstructionBackend>>::failure(
        "failed to lookup AMDGPU target: " + Error);
  }

  auto MRI =
      std::unique_ptr<llvm::MCRegisterInfo>(TheTarget->createMCRegInfo(TT));
  if (!MRI) {
    return Result<std::unique_ptr<InstructionBackend>>::failure(
        "failed to create MCRegisterInfo");
  }

  llvm::MCTargetOptions MCOptions;
  auto MAI = std::unique_ptr<llvm::MCAsmInfo>(
      TheTarget->createMCAsmInfo(*MRI, TT, MCOptions));
  if (!MAI) {
    return Result<std::unique_ptr<InstructionBackend>>::failure(
        "failed to create MCAsmInfo");
  }

  auto MCII = std::unique_ptr<llvm::MCInstrInfo>(TheTarget->createMCInstrInfo());
  if (!MCII) {
    return Result<std::unique_ptr<InstructionBackend>>::failure(
        "failed to create MCInstrInfo");
  }

  auto STI = std::unique_ptr<llvm::MCSubtargetInfo>(
      TheTarget->createMCSubtargetInfo(TT, Options.CPU, Options.Features));
  if (!STI) {
    return Result<std::unique_ptr<InstructionBackend>>::failure(
        "failed to create MCSubtargetInfo");
  }

  auto Ctx =
      std::make_unique<llvm::MCContext>(TT, MAI.get(), MRI.get(), STI.get());

  auto Disasm = std::unique_ptr<llvm::MCDisassembler>(
      TheTarget->createMCDisassembler(*STI, *Ctx));
  if (!Disasm) {
    return Result<std::unique_ptr<InstructionBackend>>::failure(
        "failed to create MCDisassembler");
  }

  auto Emitter = std::unique_ptr<llvm::MCCodeEmitter>(
      TheTarget->createMCCodeEmitter(*MCII, *Ctx));
  if (!Emitter) {
    return Result<std::unique_ptr<InstructionBackend>>::failure(
        "failed to create MCCodeEmitter");
  }

  auto Printer = std::unique_ptr<llvm::MCInstPrinter>(
      TheTarget->createMCInstPrinter(TT, 0, *MAI, *MCII, *MRI));
  if (!Printer) {
    return Result<std::unique_ptr<InstructionBackend>>::failure(
        "failed to create MCInstPrinter");
  }

  auto Backend = std::unique_ptr<LlvmBackend>(new LlvmBackend(
      std::move(Ctx), std::move(Disasm), std::move(Emitter), std::move(MCII),
      std::move(MRI), std::move(STI), std::move(MAI), std::move(Printer),
      TheTarget));
  if (auto E = Backend->discoverOpcodesAndRegisters()) {
    return failureFromError<std::unique_ptr<InstructionBackend>>(std::move(E));
  }

  std::unique_ptr<InstructionBackend> Erased = std::move(Backend);
  return Result<std::unique_ptr<InstructionBackend>>::success(std::move(Erased));
}

} // namespace amdgpu_instr_backend_llvm
