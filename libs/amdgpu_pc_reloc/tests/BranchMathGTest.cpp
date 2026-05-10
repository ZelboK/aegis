//===-- BranchMathGTest.cpp - AMDGPU PC reloc core tests -------*- C++ -*-===//

#include "amdgpu_pc_reloc/BranchMath.h"
#include "amdgpu_pc_reloc/PatchPlanner.h"

#include <gtest/gtest.h>

namespace {

class MockInstructionBackend final
    : public amdgpu_instr_backend::InstructionBackend {
public:
  const amdgpu_instr_backend::OpcodeInfo &opcodes() const override {
    return Opcodes;
  }

  amdgpu_instr_backend::Result<amdgpu_instr_backend::Instruction>
  decode(amdgpu_instr_backend::ByteView, uint64_t) const override {
    return amdgpu_instr_backend::Result<
        amdgpu_instr_backend::Instruction>::failure("not implemented");
  }

  amdgpu_instr_backend::Result<std::vector<amdgpu_instr_backend::Instruction>>
  decodeAll(amdgpu_instr_backend::ByteView, uint64_t) const override {
    return amdgpu_instr_backend::Result<
        std::vector<amdgpu_instr_backend::Instruction>>::failure(
        "not implemented");
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>>
  encode(const amdgpu_instr_backend::Instruction &) const override {
    return amdgpu_instr_backend::Result<std::vector<uint8_t>>::failure(
        "not implemented");
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>>
  encodeSBranch(int16_t DwordOffset) const override {
    return amdgpu_instr_backend::Result<std::vector<uint8_t>>::success(
        {static_cast<uint8_t>(DwordOffset & 0xFF),
         static_cast<uint8_t>((DwordOffset >> 8) & 0xFF), 0x82, 0xBF});
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>>
  encodeNop() const override {
    return amdgpu_instr_backend::Result<std::vector<uint8_t>>::success(
        {0x00, 0x00, 0x80, 0xBF});
  }

  amdgpu_instr_backend::Result<uint64_t>
  branchTarget(const amdgpu_instr_backend::Instruction &, uint64_t) const override {
    return amdgpu_instr_backend::Result<uint64_t>::failure("not implemented");
  }

  const amdgpu_instr_backend::SgprPairInfo *
  getSgprPairInfo(unsigned) const override {
    return nullptr;
  }

private:
  amdgpu_instr_backend::OpcodeInfo Opcodes;
};

} // namespace

TEST(BranchMathTest, ComputesTargetsRangesAndAlignment) {
  auto Zero = amdgpu_pc_reloc::BranchMath::encodeDwordOffset(0x1000, 0x1004);
  ASSERT_TRUE(Zero.Ok);
  EXPECT_EQ(Zero.DwordOffset, 0);
  EXPECT_EQ(amdgpu_pc_reloc::BranchMath::branchTarget(0x1000, 0), 0x1004u);

  auto Positive =
      amdgpu_pc_reloc::BranchMath::encodeDwordOffset(0x1000, 0x1010);
  ASSERT_TRUE(Positive.Ok);
  EXPECT_EQ(Positive.DwordOffset, 3);

  auto Negative =
      amdgpu_pc_reloc::BranchMath::encodeDwordOffset(0x1000, 0x0FFC);
  ASSERT_TRUE(Negative.Ok);
  EXPECT_EQ(Negative.DwordOffset, -2);

  EXPECT_TRUE(amdgpu_pc_reloc::BranchMath::inSBranchRange(32767));
  EXPECT_TRUE(amdgpu_pc_reloc::BranchMath::inSBranchRange(-32768));
  EXPECT_FALSE(amdgpu_pc_reloc::BranchMath::inSBranchRange(32768));

  auto Unaligned =
      amdgpu_pc_reloc::BranchMath::encodeDwordOffset(0x1000, 0x1002);
  EXPECT_FALSE(Unaligned.Ok);
  EXPECT_EQ(Unaligned.Diag.Code,
            amdgpu_pc_reloc::DiagnosticCode::UnalignedTarget);
}

TEST(PatchPlannerTest, PlansDirectSBranchOverwriteWithPadding) {
  MockInstructionBackend Backend;
  amdgpu_pc_reloc::PatchPlanner Planner(Backend);

  auto PlanOrErr =
      Planner.planSBranchOverwrite({0x1000, 0x1010, /*OverwriteSize=*/8});
  ASSERT_TRUE(PlanOrErr) << PlanOrErr.error();
  const auto &Plan = PlanOrErr.value();
  ASSERT_TRUE(Plan.Diagnostics.empty());
  EXPECT_EQ(Plan.DwordOffset, 3);
  EXPECT_EQ(Plan.TargetPC, 0x1010u);
  EXPECT_EQ(Plan.Bytes.size(), 8u);
}

TEST(PatchPlannerTest, RejectsInvalidSBranchOverwriteSizes) {
  MockInstructionBackend Backend;
  amdgpu_pc_reloc::PatchPlanner Planner(Backend);

  for (uint64_t Size : {0u, 2u, 6u}) {
    auto PlanOrErr = Planner.planSBranchOverwrite({0x1000, 0x1010, Size});
    EXPECT_FALSE(PlanOrErr);
  }
}

TEST(PatchPlannerTest, ReportsOutOfRangeSBranchOverwrite) {
  MockInstructionBackend Backend;
  amdgpu_pc_reloc::PatchPlanner Planner(Backend);

  auto PlanOrErr =
      Planner.planSBranchOverwrite({0x1000, 0x90000, /*OverwriteSize=*/8});
  ASSERT_TRUE(PlanOrErr) << PlanOrErr.error();
  EXPECT_FALSE(PlanOrErr.value().Diagnostics.empty());
  EXPECT_TRUE(PlanOrErr.value().Bytes.empty());
}
