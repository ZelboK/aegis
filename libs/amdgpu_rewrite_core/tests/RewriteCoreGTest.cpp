//===-- RewriteCoreGTest.cpp - rewrite core tests -------------*- C++ -*-===//

#include "amdgpu_rewrite_core/RewriteCore.h"

#include <gtest/gtest.h>

namespace {

class MockBackend final : public amdgpu_instr_backend::InstructionBackend {
public:
  const amdgpu_instr_backend::OpcodeInfo &opcodes() const override {
    return opcodes_;
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
  encodeSBranch(int16_t dwordOffset) const override {
    return amdgpu_instr_backend::Result<std::vector<uint8_t>>::success(
        {static_cast<uint8_t>(dwordOffset & 0xFF),
         static_cast<uint8_t>((dwordOffset >> 8) & 0xFF), 0x82, 0xBF});
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
  amdgpu_instr_backend::OpcodeInfo opcodes_;
};

amdgpu_rewrite_core::RewriteRequest baseRequest() {
  amdgpu_rewrite_core::RewriteRequest request;
  request.rewriteId = "rewrite-1";
  request.kernelName = "kernel";
  request.arch = "gfx942";
  request.kernelObject = 0xABC;
  request.entryPc = 0x1000;
  request.textBase = 0x1000;
  request.textOffset = 0;
  request.codeObjectBytes.assign(64, 0xCC);
  return request;
}

} // namespace

TEST(AmdgpuRewriteCoreTest, DirectPatchUsesProtoIrTrace) {
  MockBackend backend;
  amdgpu_rewrite_core::Rewriter rewriter(backend);
  auto request = baseRequest();
  request.sites.push_back({1, 0x1000, 0x1010, 4,
                           amdgpu_rewrite_core::SiteKind::globalMemory});

  amdgpu_rewrite_core::RewriteOptions options;
  options.instrumentation = amdgpu_rewrite_core::InstrumentationLevel::dryPayload;
  options.zeroSgprFlavor = amdgpu_rewrite_core::ZeroSgprFlavor::withoutVgprBump;

  auto result = rewriter.rewrite(request, options);
  ASSERT_TRUE(result) << result.error();
  ASSERT_FALSE(result.value().hasErrors());
  EXPECT_EQ(result.value().trace.rewriteId, "rewrite-1");
  EXPECT_EQ(result.value().trace.plan.selectedSites.size(), 1u);
  EXPECT_EQ(result.value().trace.plan.instrumentation,
            amdgpu_rewrite_core::InstrumentationLevel::dryPayload);
  EXPECT_EQ(result.value().trace.plan.zeroSgprFlavor,
            amdgpu_rewrite_core::ZeroSgprFlavor::withoutVgprBump);
  EXPECT_EQ(result.value().trace.patches.size(), 1u);
  EXPECT_EQ(result.value().patched.bytes[2], 0x82);
  EXPECT_FALSE(result.value().trace.invariants.empty());
}

TEST(AmdgpuRewriteCoreTest, DaisyChainRoutePatchesSiteAndCells) {
  MockBackend backend;
  amdgpu_rewrite_core::Rewriter rewriter(backend);
  auto request = baseRequest();
  request.sites.push_back({7, 0x1000, 0x1020, 4,
                           amdgpu_rewrite_core::SiteKind::globalMemory});
  request.daisyChains.push_back({7, {0x1010}});

  amdgpu_rewrite_core::RewriteOptions options;
  options.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::countingPayload;
  auto result = rewriter.rewrite(request, options);

  ASSERT_TRUE(result) << result.error();
  ASSERT_FALSE(result.value().hasErrors());
  ASSERT_EQ(result.value().trace.daisyChains.size(), 1u);
  EXPECT_EQ(result.value().trace.patches.size(), 2u);
  EXPECT_EQ(result.value().trace.patches[0].reason, "site-to-daisy-chain");
  EXPECT_EQ(result.value().trace.patches[1].reason, "daisy-chain-cell");
}

TEST(AmdgpuRewriteCoreTest, RejectsUnsupportedStrategyFamily) {
  MockBackend backend;
  amdgpu_rewrite_core::Rewriter rewriter(backend);
  auto request = baseRequest();
  amdgpu_rewrite_core::RewriteOptions options;

  auto result = rewriter.rewrite(request, options);
  ASSERT_TRUE(result) << result.error();
  EXPECT_FALSE(result.value().hasErrors());
  EXPECT_EQ(result.value().trace.plan.layout,
            amdgpu_rewrite_core::RewriteLayout::singleKernelClone);
  EXPECT_EQ(result.value().trace.plan.registerMode,
            amdgpu_rewrite_core::RegisterMode::zeroSgpr);
}
