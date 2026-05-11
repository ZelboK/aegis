//===-- LlvmBackendGTest.cpp - LLVM backend relocation tests ----*- C++ -*-===//

#include "amdgpu_instr_backend_llvm/LlvmBackend.h"
#include "amdgpu_pc_reloc/BranchMath.h"
#include "amdgpu_pc_reloc/PatchPlanner.h"
#include "amdgpu_pc_reloc/Relocator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using amdgpu_instr_backend::Instruction;
using amdgpu_instr_backend::InstructionBackend;
using amdgpu_instr_backend::Operand;
using amdgpu_instr_backend::CountingRecordWrite;

std::string readFile(const std::filesystem::path &Path) {
  std::ifstream Input(Path);
  return std::string(std::istreambuf_iterator<char>(Input),
                     std::istreambuf_iterator<char>());
}

class LlvmBackendRelocTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto BackendOrErr = amdgpu_instr_backend_llvm::createLlvmBackend();
    if (!BackendOrErr) {
      GTEST_SKIP() << BackendOrErr.error();
    }
    Backend = BackendOrErr.takeValue();
  }

  std::vector<uint8_t> encode(const Instruction &Inst) {
    auto BytesOrErr = Backend->encode(Inst);
    if (!BytesOrErr) {
      ADD_FAILURE() << BytesOrErr.error();
      return {};
    }
    return BytesOrErr.takeValue();
  }

  std::vector<uint8_t> makeGetpcAdd(uint64_t GetPC, uint64_t Target) {
    uint8_t GetpcBytes[] = {0x00, 0x1C, 0x80, 0xBE};
    auto GetpcOrErr = Backend->decode(GetpcBytes, GetPC);
    if (!GetpcOrErr) {
      ADD_FAILURE() << GetpcOrErr.error();
      return {};
    }
    Instruction Getpc = GetpcOrErr.takeValue();
    EXPECT_FALSE(Getpc.Operands.empty());
    if (Getpc.Operands.empty() || !Getpc.Operands[0].isReg()) {
      return {};
    }
    const auto *Pair = Backend->getSgprPairInfo(Getpc.Operands[0].Register);
    EXPECT_NE(Pair, nullptr);
    if (!Pair) {
      return {};
    }

    int64_t Addend =
        static_cast<int64_t>(Target) - static_cast<int64_t>(GetPC + 4);
    uint64_t AddendU = static_cast<uint64_t>(Addend);
    int32_t Lo = static_cast<int32_t>(AddendU & 0xFFFFFFFFu);
    int32_t Hi = static_cast<int32_t>((AddendU >> 32) & 0xFFFFFFFFu);

    Instruction Add;
    Add.BackendOpcode = Backend->opcodes().SAddU32;
    Add.Operands = {
        Operand::reg(Pair->LoReg),
        Operand::reg(Pair->LoReg),
        Operand::imm(Lo),
    };

    Instruction Addc;
    Addc.BackendOpcode = Backend->opcodes().SAddcU32;
    Addc.Operands = {
        Operand::reg(Pair->HiReg),
        Operand::reg(Pair->HiReg),
        Operand::imm(Hi),
    };

    std::vector<uint8_t> Result = encode(Getpc);
    auto AddBytes = encode(Add);
    auto AddcBytes = encode(Addc);
    Result.insert(Result.end(), AddBytes.begin(), AddBytes.end());
    Result.insert(Result.end(), AddcBytes.begin(), AddcBytes.end());
    return Result;
  }

  static bool extractConstant(const Operand &Op, int64_t &Out) {
    if (!Op.isImm()) {
      return false;
    }
    Out = Op.Immediate;
    return true;
  }

  std::unique_ptr<InstructionBackend> Backend;
};

} // namespace

TEST_F(LlvmBackendRelocTest, PlansDirectSBranchOverwrite) {
  amdgpu_pc_reloc::PatchPlanner Planner(*Backend);
  auto PlanOrErr =
      Planner.planSBranchOverwrite({0x1000, 0x1010, /*OverwriteSize=*/8});
  ASSERT_TRUE(PlanOrErr) << PlanOrErr.error();
  ASSERT_TRUE(PlanOrErr.value().Diagnostics.empty());
  EXPECT_EQ(PlanOrErr.value().Bytes.size(), 8u);

  auto Decoded = Backend->decode(PlanOrErr.value().Bytes, 0x1000);
  ASSERT_TRUE(Decoded) << Decoded.error();
  auto Target = Backend->branchTarget(Decoded.value(), 0x1000);
  ASSERT_TRUE(Target) << Target.error();
  EXPECT_EQ(Target.value(), 0x1010u);
}

TEST_F(LlvmBackendRelocTest, PlansSBranchOverwritePaddingSizes) {
  amdgpu_pc_reloc::PatchPlanner Planner(*Backend);
  for (uint64_t Size : {4u, 8u, 16u}) {
    auto PlanOrErr = Planner.planSBranchOverwrite({0x1000, 0x1010, Size});
    ASSERT_TRUE(PlanOrErr) << PlanOrErr.error();
    ASSERT_TRUE(PlanOrErr.value().Diagnostics.empty());
    EXPECT_EQ(PlanOrErr.value().Bytes.size(), Size);

    auto Decoded = Backend->decode(PlanOrErr.value().Bytes, 0x1000);
    ASSERT_TRUE(Decoded) << Decoded.error();
    auto Target = Backend->branchTarget(Decoded.value(), 0x1000);
    ASSERT_TRUE(Target) << Target.error();
    EXPECT_EQ(Target.value(), 0x1010u);
  }
}

TEST_F(LlvmBackendRelocTest, RelocatesBranchTargetsInCopiedRange) {
  auto Br = Backend->encodeSBranch(3);
  ASSERT_TRUE(Br) << Br.error();
  std::vector<uint8_t> Code = Br.takeValue();

  amdgpu_pc_reloc::Relocator R(*Backend);
  amdgpu_pc_reloc::RelocateRangeRequest Req;
  Req.Bytes = Code;
  Req.SourceBase = 0x1000;
  Req.DestinationBase = 0x2000;
  Req.MovingRanges = {{0x1000, 0x1020}};
  Req.ErrorOnUnmatchedGetPC = false;
  auto Relocated = R.relocateRange(Req);
  ASSERT_TRUE(Relocated) << Relocated.error();
  ASSERT_FALSE(Relocated.value().hasErrors());

  auto Decoded = Backend->decode(Relocated.value().Bytes, 0x2000);
  ASSERT_TRUE(Decoded) << Decoded.error();
  auto Target = Backend->branchTarget(Decoded.value(), 0x2000);
  ASSERT_TRUE(Target) << Target.error();
  EXPECT_EQ(Target.value(), 0x2010u);
}

TEST_F(LlvmBackendRelocTest, RelocatesExternalBranchTargets) {
  auto Dword = amdgpu_pc_reloc::BranchMath::encodeDwordOffset(0x1000, 0x3000);
  ASSERT_TRUE(Dword.Ok);
  auto Br = Backend->encodeSBranch(static_cast<int16_t>(Dword.DwordOffset));
  ASSERT_TRUE(Br) << Br.error();

  amdgpu_pc_reloc::Relocator R(*Backend);
  amdgpu_pc_reloc::RelocateRangeRequest Req;
  Req.Bytes = Br.value();
  Req.SourceBase = 0x1000;
  Req.DestinationBase = 0x2000;
  Req.MovingRanges = {{0x1000, 0x1004}};
  Req.ErrorOnUnmatchedGetPC = false;
  auto Relocated = R.relocateRange(Req);
  ASSERT_TRUE(Relocated) << Relocated.error();
  ASSERT_FALSE(Relocated.value().hasErrors());

  auto Decoded = Backend->decode(Relocated.value().Bytes, 0x2000);
  ASSERT_TRUE(Decoded) << Decoded.error();
  auto Target = Backend->branchTarget(Decoded.value(), 0x2000);
  ASSERT_TRUE(Target) << Target.error();
  EXPECT_EQ(Target.value(), 0x3000u);
}

TEST_F(LlvmBackendRelocTest, RewritesExternalGetpcAddendWhenCopied) {
  std::vector<uint8_t> Code = makeGetpcAdd(0x1000, 0x3000);
  ASSERT_FALSE(Code.empty());

  amdgpu_pc_reloc::Relocator R(*Backend);
  amdgpu_pc_reloc::RelocateRangeRequest Req;
  Req.Bytes = Code;
  Req.SourceBase = 0x1000;
  Req.DestinationBase = 0x1100;
  Req.MovingRanges = {{0x1000, 0x1000 + Code.size()}};
  auto Relocated = R.relocateRange(Req);
  ASSERT_TRUE(Relocated) << Relocated.error();
  ASSERT_FALSE(Relocated.value().hasErrors());
  ASSERT_EQ(Relocated.value().Records.size(), 1u);
  EXPECT_EQ(Relocated.value().Records[0].Kind, "getpc-addend");
  EXPECT_EQ(Relocated.value().Records[0].TargetAfter, 0x3000u);

  auto Insts = Backend->decodeAll(Relocated.value().Bytes, 0x1100);
  ASSERT_TRUE(Insts) << Insts.error();
  ASSERT_GE(Insts.value().size(), 3u);
  int64_t Lo = 0;
  int64_t Hi = 0;
  ASSERT_TRUE(extractConstant(Insts.value()[1].Operands[2], Lo));
  ASSERT_TRUE(extractConstant(Insts.value()[2].Operands[2], Hi));
  uint64_t AddendU =
      (static_cast<uint64_t>(static_cast<uint32_t>(Hi)) << 32) |
      static_cast<uint32_t>(Lo);
  int64_t Addend = static_cast<int64_t>(AddendU);
  EXPECT_EQ(static_cast<int64_t>(0x1104) + Addend, 0x3000);
}

TEST_F(LlvmBackendRelocTest, RelocationSessionSupportsRepeatedCopies) {
  std::vector<uint8_t> Code;
  for (unsigned I = 0; I < 64; ++I) {
    auto Br = Backend->encodeSBranch(0);
    ASSERT_TRUE(Br) << Br.error();
    auto Bytes = Br.takeValue();
    Code.insert(Code.end(), Bytes.begin(), Bytes.end());
  }

  amdgpu_pc_reloc::RelocationSession Session(*Backend, Code, 0x4000);
  auto Decode = Session.decode();
  ASSERT_TRUE(Decode) << Decode.error();
  ASSERT_EQ(Session.instructions().size(), 64u);

  for (unsigned I = 0; I < 8; ++I) {
    auto Relocated =
        Session.relocateCopy(0x4000, Code.size(), 0x8000 + I * 0x1000);
    ASSERT_TRUE(Relocated) << Relocated.error();
    ASSERT_FALSE(Relocated.value().hasErrors());
    EXPECT_EQ(Relocated.value().Records.size(), 64u);
  }
}

TEST_F(LlvmBackendRelocTest, EmitsCountingRecordWriteSequence) {
  CountingRecordWrite Request;
  Request.bufferAddress = 0x100000;
  Request.bufferSize = 24;
  Request.siteId = 7;
  Request.value = 1;
  Request.addressVgprIndex = 8;
  Request.saveVgprIndex = 9;
  Request.dataVgprIndex = 10;

  auto Bytes = Backend->encodeCountingRecordWrite(Request);
  ASSERT_TRUE(Bytes) << Bytes.error();
  ASSERT_FALSE(Bytes.value().empty());

  auto Insts = Backend->decodeAll(Bytes.value(), 0x8000);
  ASSERT_TRUE(Insts) << Insts.error();
  ASSERT_GE(Insts.value().size(), 8u);
  bool SawGlobalStore = false;
  for (const auto &Inst : Insts.value()) {
    if (Inst.MayStore && Inst.Memory == Instruction::MemoryKind::global) {
      SawGlobalStore = true;
      break;
    }
  }
  EXPECT_TRUE(SawGlobalStore);
}

TEST_F(LlvmBackendRelocTest, EmitsCountingRecordWriteWithAgprSpill) {
  CountingRecordWrite Request;
  Request.bufferAddress = 0x100000;
  Request.bufferSize = 24;
  Request.siteId = 7;
  Request.value = 1;
  Request.addressVgprIndex = 5;
  Request.saveVgprIndex = 6;
  Request.dataVgprIndex = 7;
  Request.useAgprSpill = true;
  Request.agprSpillBaseIndex = 0;

  auto Bytes = Backend->encodeCountingRecordWrite(Request);
  ASSERT_TRUE(Bytes) << Bytes.error();
  ASSERT_FALSE(Bytes.value().empty());

  auto Insts = Backend->decodeAll(Bytes.value(), 0x8000);
  ASSERT_TRUE(Insts) << Insts.error();
  bool SawAccWrite = false;
  bool SawAccRead = false;
  for (const auto &Inst : Insts.value()) {
    if (Inst.Mnemonic == "v_accvgpr_write_b32") {
      SawAccWrite = true;
    }
    if (Inst.Mnemonic == "v_accvgpr_read_b32") {
      SawAccRead = true;
    }
  }
  EXPECT_TRUE(SawAccWrite);
  EXPECT_TRUE(SawAccRead);
}

TEST_F(LlvmBackendRelocTest, RejectsCountingRecordWriteWithoutBuffer) {
  CountingRecordWrite Request;
  Request.bufferAddress = 0;
  Request.bufferSize = 24;
  Request.addressVgprIndex = 8;
  Request.saveVgprIndex = 9;
  Request.dataVgprIndex = 10;

  auto Bytes = Backend->encodeCountingRecordWrite(Request);
  ASSERT_FALSE(Bytes);
  EXPECT_NE(Bytes.error().find("valid profiling buffer"), std::string::npos);
}

TEST(AmdgpuInstrBackendLlvmBoundaryTest, PublicHeadersAvoidLlvmTypes) {
  const std::filesystem::path IncludeDir =
      std::filesystem::path(NEW_AEGIS_AMDGPU_INSTR_BACKEND_LLVM_DIR) /
      "include";
  const std::vector<std::string> Forbidden = {
      "#include \"llvm/", "#include <llvm/", "llvm::", "MCInst",
      "MCDisassembler"};

  for (const auto &Entry :
       std::filesystem::recursive_directory_iterator(IncludeDir)) {
    if (!Entry.is_regular_file()) {
      continue;
    }
    std::string Contents = readFile(Entry.path());
    for (const std::string &Token : Forbidden) {
      EXPECT_EQ(Contents.find(Token), std::string::npos)
          << Entry.path() << " contains forbidden token " << Token;
    }
  }
}
