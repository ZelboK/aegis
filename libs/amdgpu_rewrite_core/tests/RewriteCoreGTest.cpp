//===-- RewriteCoreGTest.cpp - rewrite core tests -------------*- C++ -*-===//

#include "amdgpu_rewrite_core/RewriteCore.h"
#include "amdgpu_rewrite_core/CodeObjectModel.h"
#include "amdgpu_rewrite_core/SiteAnalysis.h"

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
  decodeAll(amdgpu_instr_backend::ByteView, uint64_t baseAddress) const override {
    amdgpu_instr_backend::Instruction branch;
    branch.BackendOpcode = 1;
    branch.Address = baseAddress;
    branch.Size = 4;
    branch.IsPCRelativeBranch = true;
    branch.Operands.push_back(amdgpu_instr_backend::Operand::imm(3));
    return amdgpu_instr_backend::Result<
        std::vector<amdgpu_instr_backend::Instruction>>::success({branch});
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
  branchTarget(const amdgpu_instr_backend::Instruction &inst,
               uint64_t currentPc) const override {
    if (inst.Operands.empty() || !inst.Operands.back().isImm()) {
      return amdgpu_instr_backend::Result<uint64_t>::failure(
          "missing branch immediate");
    }
    return amdgpu_instr_backend::Result<uint64_t>::success(
        static_cast<uint64_t>(static_cast<int64_t>(currentPc + 4) +
                              inst.Operands.back().Immediate * 4));
  }

  const amdgpu_instr_backend::SgprPairInfo *
  getSgprPairInfo(unsigned) const override {
    return nullptr;
  }

private:
  amdgpu_instr_backend::OpcodeInfo opcodes_;
};

class MockCodeObjectParser final : public amdgpu_code_object::CodeObjectParser {
public:
  amdgpu_instr_backend::Result<amdgpu_code_object::ParsedKernelCode>
  parseKernel(const amdgpu_code_object::ParseRequest &) const override {
    amdgpu_code_object::ParsedKernelCode kernel;
    kernel.name = "kernel";
    kernel.arch = "gfx942";
    kernel.entryPc = 0x2000;
    kernel.textBase = 0x2000;
    kernel.textOffset = 16;
    kernel.textSize = 64;
    return amdgpu_instr_backend::Result<
        amdgpu_code_object::ParsedKernelCode>::success(kernel);
  }
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
  request.textSize = 64;
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

TEST(AmdgpuRewriteCoreTest, ParsesCodeObjectModel) {
  auto request = baseRequest();
  auto parsed = amdgpu_rewrite_core::parseCodeObject(
      amdgpu_rewrite_core::parseRequestFromRewriteRequest(request));
  ASSERT_TRUE(parsed) << parsed.error();
  EXPECT_EQ(parsed.value().kernel.name, "kernel");
  EXPECT_EQ(parsed.value().kernel.textRange.start, 0x1000u);
  EXPECT_EQ(parsed.value().kernel.textRange.end, 0x1040u);
  EXPECT_FALSE(parsed.value().kernel.originalBytesHash.empty());
}

TEST(AmdgpuRewriteCoreTest, CodeObjectModelPrefersParserMetadata) {
  MockCodeObjectParser parser;
  auto request = baseRequest();
  request.codeObjectParser = &parser;
  request.codeObjectBytes.assign(128, 0xCC);

  auto parsed = amdgpu_rewrite_core::parseCodeObject(
      amdgpu_rewrite_core::parseRequestFromRewriteRequest(request));
  ASSERT_TRUE(parsed) << parsed.error();
  EXPECT_EQ(parsed.value().kernel.entryPc, 0x2000u);
  EXPECT_EQ(parsed.value().kernel.textRange.start, 0x2000u);
  EXPECT_EQ(parsed.value().kernel.textRange.end, 0x2040u);
  EXPECT_EQ(parsed.value().kernel.textOffset, 16u);
}

TEST(AmdgpuRewriteCoreTest, SiteAnalysisSelectsDecodedBranchSites) {
  MockBackend backend;
  auto request = baseRequest();
  auto parsed = amdgpu_rewrite_core::parseCodeObject(
      amdgpu_rewrite_core::parseRequestFromRewriteRequest(request));
  ASSERT_TRUE(parsed) << parsed.error();

  auto analysis = amdgpu_rewrite_core::analyzeSites(parsed.value(), backend);
  ASSERT_TRUE(analysis) << analysis.error();
  ASSERT_EQ(analysis.value().rewriteSites.size(), 1u);
  EXPECT_EQ(analysis.value().rewriteSites[0].patchPc, 0x1000u);
  EXPECT_EQ(analysis.value().rewriteSites[0].targetPc, 0x1010u);
  EXPECT_EQ(analysis.value().siteModels[0].decision,
            "selected: PC-relative branch candidate");
}

TEST(AmdgpuRewriteCoreTest, AutoAnalyzedNoopPatchUsesDecodedSites) {
  MockBackend backend;
  amdgpu_rewrite_core::Rewriter rewriter(backend);
  auto request = baseRequest();

  amdgpu_rewrite_core::RewriteOptions options;
  options.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::noopPatch;

  auto result = rewriter.rewrite(request, options);
  ASSERT_TRUE(result) << result.error();
  ASSERT_FALSE(result.value().hasErrors());
  EXPECT_EQ(result.value().trace.analyzedSites.size(), 1u);
  EXPECT_EQ(result.value().trace.plan.selectedSites.size(), 1u);
  EXPECT_EQ(result.value().trace.patches.size(), 1u);
  EXPECT_EQ(result.value().trace.patches[0].reason, "direct-site-patch");
  EXPECT_EQ(result.value().trace.plan.instrumentation,
            amdgpu_rewrite_core::InstrumentationLevel::noopPatch);
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
