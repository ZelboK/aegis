//===-- RewriteCoreGTest.cpp - rewrite core tests -------------*- C++ -*-===//

#include "amdgpu_rewrite_core/RewriteCore.h"
#include "amdgpu_rewrite_core/CodeObjectMutation.h"
#include "amdgpu_rewrite_core/CodeObjectModel.h"
#include "amdgpu_rewrite_core/CountingPayloadAbi.h"
#include "amdgpu_rewrite_core/SiteAnalysis.h"

#include <gtest/gtest.h>

#include <optional>

namespace {

class MockBackend final : public amdgpu_instr_backend::InstructionBackend {
public:
  explicit MockBackend(bool decodeMemorySite = false)
      : decodeMemorySite_(decodeMemorySite) {}

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
    if (decodeMemorySite_) {
      amdgpu_instr_backend::Instruction memory;
      memory.BackendOpcode = 2;
      memory.Address = baseAddress;
      memory.Size = 8;
      memory.MayLoad = true;
      memory.Mnemonic = "global_load_dword";
      memory.Memory =
          amdgpu_instr_backend::Instruction::MemoryKind::global;
      memory.AddressRegisters.push_back(10);
      return amdgpu_instr_backend::Result<
          std::vector<amdgpu_instr_backend::Instruction>>::success({memory});
    }

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

  amdgpu_instr_backend::Result<std::vector<uint8_t>>
  encodeCountingRecordWrite(
      const amdgpu_instr_backend::CountingRecordWrite &request) const override {
    lastCountingRequest_ = request;
    return encodeNop();
  }

  const std::optional<amdgpu_instr_backend::CountingRecordWrite> &
  lastCountingRequest() const {
    return lastCountingRequest_;
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
  bool decodeMemorySite_ = false;
  mutable std::optional<amdgpu_instr_backend::CountingRecordWrite>
      lastCountingRequest_;
  amdgpu_instr_backend::OpcodeInfo opcodes_;
};

class MockCodeObjectParser final : public amdgpu_code_object::CodeObjectParser {
public:
  explicit MockCodeObjectParser(bool includeDescriptor = false,
                                uint32_t accumOffset = 0)
      : includeDescriptor_(includeDescriptor), accumOffset_(accumOffset) {}

  amdgpu_instr_backend::Result<amdgpu_code_object::ParsedKernelCode>
  parseKernel(const amdgpu_code_object::ParseRequest &) const override {
    amdgpu_code_object::ParsedKernelCode kernel;
    kernel.name = "kernel";
    kernel.arch = "gfx942";
    kernel.entryPc = 0x2000;
    kernel.textBase = 0x2000;
    kernel.textOffset = 16;
    kernel.textSize = 64;
    if (includeDescriptor_) {
      amdgpu_code_object::DescriptorFacts descriptor;
      descriptor.present = true;
      descriptor.fileOffset = 96;
      descriptor.size = 64;
      if (accumOffset_ != 0) {
        descriptor.computePgmRsrc3 = (accumOffset_ / 4) - 1;
      }
      descriptor.computePgmRsrc1 = 0;
      descriptor.vgprCount = 8;
      descriptor.sgprCount = 8;
      descriptor.vgprGranularity = 8;
      kernel.descriptor = descriptor;
    }
    return amdgpu_instr_backend::Result<
        amdgpu_code_object::ParsedKernelCode>::success(kernel);
  }

private:
  bool includeDescriptor_ = false;
  uint32_t accumOffset_ = 0;
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
  ASSERT_EQ(result.value().trace.trampolines.size(), 1u);
  EXPECT_EQ(result.value().trace.trampolines[0].description,
            "DryPayload control-flow detour");
  EXPECT_EQ(result.value().trace.patches[0].reason, "dry-payload-detour");
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

TEST(AmdgpuRewriteCoreTest, CodeObjectMutationAppendsRawTextBytes) {
  amdgpu_rewrite_core::TextAppendRequest request;
  request.codeObjectBytes = {0xAA, 0xBB, 0xCC, 0xDD};
  request.textBase = 0x1000;
  request.textOffset = 0;
  request.textSize = 4;
  request.bytesToAppend = {0x11, 0x22};

  auto result = amdgpu_rewrite_core::appendToText(request);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(result.value().appendedPc, 0x1004u);
  EXPECT_EQ(result.value().textSize, 6u);
  EXPECT_EQ(result.value().codeObjectBytes,
            std::vector<uint8_t>({0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22}));
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

TEST(AmdgpuRewriteCoreTest, DescriptorUpdateBumpsVgprCountWhenRequested) {
  MockBackend backend;
  MockCodeObjectParser parser(/*includeDescriptor=*/true);
  amdgpu_rewrite_core::Rewriter rewriter(backend);
  auto request = baseRequest();
  request.codeObjectParser = &parser;
  request.codeObjectBytes.assign(160, 0);
  request.textOffset = 16;
  request.textSize = 64;
  request.sites.push_back({1, 0x2000, 0x2004, 4,
                           amdgpu_rewrite_core::SiteKind::globalMemory});

  amdgpu_rewrite_core::RewriteOptions options;
  options.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::dryPayload;
  options.zeroSgprFlavor = amdgpu_rewrite_core::ZeroSgprFlavor::withVgprBump;

  auto result = rewriter.rewrite(request, options);
  ASSERT_TRUE(result) << result.error();
  ASSERT_FALSE(result.value().hasErrors());
  ASSERT_EQ(result.value().trace.descriptorUpdates.size(), 2u);
  EXPECT_EQ(result.value().trace.descriptorUpdates[0].field, "vgpr_count");
  EXPECT_EQ(result.value().trace.descriptorUpdates[0].oldValue, 8u);
  EXPECT_EQ(result.value().trace.descriptorUpdates[0].newValue, 16u);
  EXPECT_EQ(result.value().patched.bytes[96 + 48 + 12], 1u);
}

TEST(AmdgpuRewriteCoreTest, CountingPayloadWithAccumOffsetUsesAgprSpill) {
  MockBackend backend;
  MockCodeObjectParser parser(/*includeDescriptor=*/true, /*accumOffset=*/8);
  amdgpu_rewrite_core::Rewriter rewriter(backend);
  auto request = baseRequest();
  uint64_t counter = 0;
  request.codeObjectParser = &parser;
  request.codeObjectBytes.assign(160, 0);
  request.textOffset = 16;
  request.textSize = 64;
  request.profilingBufferAddress = reinterpret_cast<uint64_t>(&counter);
  request.profilingBufferSize =
      amdgpu_rewrite_core::countingPayloadRecordV1Size;
  request.sites.push_back({9, 0x2000, 0x2010, 4,
                           amdgpu_rewrite_core::SiteKind::globalMemory});

  amdgpu_rewrite_core::RewriteOptions options;
  options.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::countingPayload;
  options.zeroSgprFlavor = amdgpu_rewrite_core::ZeroSgprFlavor::withVgprBump;

  auto result = rewriter.rewrite(request, options);
  ASSERT_TRUE(result) << result.error();
  ASSERT_FALSE(result.value().hasErrors());
  ASSERT_TRUE(backend.lastCountingRequest().has_value());
  EXPECT_EQ(backend.lastCountingRequest()->addressVgprIndex, 5u);
  EXPECT_EQ(backend.lastCountingRequest()->saveVgprIndex, 6u);
  EXPECT_EQ(backend.lastCountingRequest()->dataVgprIndex, 7u);
  EXPECT_TRUE(backend.lastCountingRequest()->useAgprSpill);
  EXPECT_EQ(backend.lastCountingRequest()->agprSpillBaseIndex, 0u);
  EXPECT_EQ(result.value().trace.kernel.computePgmRsrc3, 1u);
  EXPECT_EQ(result.value().patched.bytes[96 + 44], 0u);
  ASSERT_GE(result.value().trace.descriptorUpdates.size(), 3u);
  EXPECT_EQ(result.value().trace.descriptorUpdates[0].field,
            "agpr_spill_base");
}

TEST(AmdgpuRewriteCoreTest, SiteAnalysisSelectsSupportedMemorySites) {
  MockBackend backend(/*decodeMemorySite=*/true);
  auto request = baseRequest();
  auto parsed = amdgpu_rewrite_core::parseCodeObject(
      amdgpu_rewrite_core::parseRequestFromRewriteRequest(request));
  ASSERT_TRUE(parsed) << parsed.error();

  auto analysis = amdgpu_rewrite_core::analyzeSites(parsed.value(), backend);
  ASSERT_TRUE(analysis) << analysis.error();
  ASSERT_EQ(analysis.value().rewriteSites.size(), 1u);
  EXPECT_EQ(analysis.value().rewriteSites[0].patchPc, 0x1000u);
  EXPECT_EQ(analysis.value().rewriteSites[0].targetPc, 0x1008u);
  EXPECT_EQ(analysis.value().rewriteSites[0].kind,
            amdgpu_rewrite_core::SiteKind::globalMemory);
  EXPECT_EQ(analysis.value().siteModels[0].decision,
            "selected: supported memory instruction");
}

TEST(AmdgpuRewriteCoreTest, SiteAnalysisRejectsControlFlowBranches) {
  MockBackend backend;
  auto request = baseRequest();
  auto parsed = amdgpu_rewrite_core::parseCodeObject(
      amdgpu_rewrite_core::parseRequestFromRewriteRequest(request));
  ASSERT_TRUE(parsed) << parsed.error();

  auto analysis = amdgpu_rewrite_core::analyzeSites(parsed.value(), backend);
  ASSERT_TRUE(analysis) << analysis.error();
  EXPECT_TRUE(analysis.value().rewriteSites.empty());
  ASSERT_EQ(analysis.value().siteModels.size(), 1u);
  EXPECT_EQ(analysis.value().siteModels[0].decision,
            "rejected: control-flow is not a profiling site");
}

TEST(AmdgpuRewriteCoreTest, AutoAnalyzedNoopPatchPreservesBytes) {
  MockBackend backend;
  amdgpu_rewrite_core::Rewriter rewriter(backend);
  auto request = baseRequest();
  auto originalBytes = request.codeObjectBytes;

  amdgpu_rewrite_core::RewriteOptions options;
  options.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::noopPatch;

  auto result = rewriter.rewrite(request, options);
  ASSERT_TRUE(result) << result.error();
  ASSERT_FALSE(result.value().hasErrors());
  EXPECT_EQ(result.value().trace.analyzedSites.size(), 1u);
  EXPECT_TRUE(result.value().trace.plan.selectedSites.empty());
  EXPECT_TRUE(result.value().trace.patches.empty());
  EXPECT_EQ(result.value().patched.bytes, originalBytes);
  EXPECT_EQ(result.value().trace.plan.instrumentation,
            amdgpu_rewrite_core::InstrumentationLevel::noopPatch);
}

TEST(AmdgpuRewriteCoreTest, CountingPayloadRecordsPayloadModel) {
  MockBackend backend;
  MockCodeObjectParser parser(/*includeDescriptor=*/true);
  amdgpu_rewrite_core::Rewriter rewriter(backend);
  auto request = baseRequest();
  uint64_t counter = 0;
  request.codeObjectParser = &parser;
  request.codeObjectBytes.assign(160, 0);
  request.textOffset = 16;
  request.textSize = 64;
  request.profilingBufferAddress = reinterpret_cast<uint64_t>(&counter);
  request.profilingBufferSize =
      amdgpu_rewrite_core::countingPayloadRecordV1Size;
  request.sites.push_back({9, 0x2000, 0x2010, 4,
                           amdgpu_rewrite_core::SiteKind::globalMemory});

  amdgpu_rewrite_core::RewriteOptions options;
  options.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::countingPayload;

  auto result = rewriter.rewrite(request, options);
  ASSERT_TRUE(result) << result.error();
  ASSERT_FALSE(result.value().hasErrors());
  ASSERT_GE(result.value().trace.payloads.size(), 2u);
  EXPECT_EQ(result.value().trace.payloads.back().level,
            amdgpu_rewrite_core::InstrumentationLevel::countingPayload);
  EXPECT_EQ(result.value().trace.payloads.back().abi,
            "CountingPayloadRecordV1:baked-buffer-address");
  EXPECT_EQ(result.value().trace.payloads.back().profilingBufferAddress,
            reinterpret_cast<uint64_t>(&counter));
  ASSERT_EQ(result.value().trace.trampolines.size(), 1u);
  EXPECT_EQ(result.value().trace.trampolines[0].byteSize, 12u);
  EXPECT_EQ(result.value().trace.patches[0].reason,
            "counting-payload-detour");
}

TEST(AmdgpuRewriteCoreTest, DaisyChainRoutePatchesSiteAndCells) {
  MockBackend backend;
  MockCodeObjectParser parser(/*includeDescriptor=*/true);
  amdgpu_rewrite_core::Rewriter rewriter(backend);
  auto request = baseRequest();
  uint64_t counter = 0;
  request.codeObjectParser = &parser;
  request.codeObjectBytes.assign(160, 0);
  request.textOffset = 16;
  request.textSize = 64;
  request.profilingBufferAddress = reinterpret_cast<uint64_t>(&counter);
  request.profilingBufferSize =
      amdgpu_rewrite_core::countingPayloadRecordV1Size;
  request.sites.push_back({7, 0x2000, 0x2020, 4,
                           amdgpu_rewrite_core::SiteKind::globalMemory});
  request.daisyChains.push_back({7, {0x2010}});

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
