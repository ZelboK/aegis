//===-- RuntimeGTest.cpp - runtime composition tests ----------*- C++ -*-===//

#include "aegisbit_runtime/Runtime.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

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

  amdgpu_instr_backend::Result<std::vector<uint8_t>> encodeNop() const override {
    return amdgpu_instr_backend::Result<std::vector<uint8_t>>::success(
        {0x00, 0x00, 0x80, 0xBF});
  }

  amdgpu_instr_backend::Result<uint64_t>
  branchTarget(const amdgpu_instr_backend::Instruction &inst,
               uint64_t currentPc) const override {
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

class FakeKernelLoader final : public hsa_kernel_loader::KernelLoader {
public:
  amdgpu_instr_backend::Result<hsa_kernel_loader::LoadedKernel>
  loadKernel(const hsa_kernel_loader::LoadRequest &request) override {
    loadCount++;
    lastRequest = request;
    if (!error.empty()) {
      return amdgpu_instr_backend::Result<
          hsa_kernel_loader::LoadedKernel>::failure(error);
    }

    hsa_kernel_loader::LoadedKernel loaded;
    loaded.kernelName = request.kernelName;
    loaded.kernelObject = patchedKernelObject;
    loaded.executableHandle = 0xE;
    loaded.loadedCodeObjectHandle = 0xC;
    return amdgpu_instr_backend::Result<
        hsa_kernel_loader::LoadedKernel>::success(loaded);
  }

  uint64_t patchedKernelObject = 0xDEF;
  uint32_t loadCount = 0;
  std::string error;
  hsa_kernel_loader::LoadRequest lastRequest;
};

aegis::hsa_intercept::CapturedKernelSymbol symbol() {
  aegis::hsa_intercept::CapturedKernelSymbol out;
  out.codeObjectId = 1;
  out.kernelName = "kernel";
  out.kernelObject = 0xABC;
  return out;
}

aegis::hsa_intercept::DispatchEvent dispatch() {
  aegis::hsa_intercept::DispatchEvent event;
  event.originalKernelObject = 0xABC;
  event.packet.kernelObject = 0xABC;
  event.originalKernargAddress = 0x100;
  event.packet.kernargAddress = 0x100;
  return event;
}

} // namespace

TEST(AegisbitRuntimeTest, RecordsDispatchTraceWithoutBackend) {
  aegisbit_runtime::Runtime runtime;
  runtime.handleKernelSymbol(symbol());
  auto event = dispatch();

  auto decision = runtime.handleDispatch(event);
  EXPECT_EQ(decision, aegis::hsa_intercept::DispatchDecision::proceed);
  auto traces = runtime.dispatchTraces();
  ASSERT_EQ(traces.size(), 1u);
  EXPECT_EQ(traces[0].status, "no-backend");
  EXPECT_EQ(traces[0].originalKernelObject, 0xABCu);
  EXPECT_EQ(runtime.stats().observedDispatches, 1u);
}

TEST(AegisbitRuntimeTest, CapturesCodeObjectsAndKernelSymbols) {
  aegisbit_runtime::Runtime runtime;
  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes = {1, 2, 3};

  runtime.handleCodeObject(object);
  runtime.handleKernelSymbol(symbol());

  auto stats = runtime.stats();
  EXPECT_EQ(stats.capturedCodeObjects, 1u);
  EXPECT_EQ(stats.capturedKernelSymbols, 1u);
}

TEST(AegisbitRuntimeTest, DispatchWithBackendBuildsRealRewriteRequest) {
  MockBackend backend;
  aegisbit_runtime::Runtime runtime({}, &backend);
  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes.assign(64, 0xCC);
  object.loadBase = 0x1000;
  object.loadSize = object.bytes.size();
  runtime.handleCodeObject(object);
  runtime.handleKernelSymbol(symbol());

  auto event = dispatch();
  auto decision = runtime.handleDispatch(event);
  EXPECT_EQ(decision, aegis::hsa_intercept::DispatchDecision::proceed);

  auto stats = runtime.stats();
  EXPECT_EQ(stats.rewriteAttempts, 1u);
  auto lastRewrite = runtime.lastRewriteResult();
  ASSERT_TRUE(lastRewrite.has_value());
  EXPECT_EQ(lastRewrite->trace.kernel.textRange.start, 0x1000u);
  EXPECT_EQ(lastRewrite->trace.plan.selectedSites.size(), 1u);
  ASSERT_EQ(runtime.dispatchTraces().size(), 1u);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "rewritten");
}

TEST(AegisbitRuntimeTest, DispatchUsesParsedKernelMetadataWhenParserExists) {
  MockBackend backend;
  MockCodeObjectParser parser;
  aegisbit_runtime::Runtime runtime({}, &backend, &parser);
  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes.assign(128, 0xCC);
  object.loadBase = 0x1000;
  object.loadSize = object.bytes.size();
  runtime.handleCodeObject(object);
  runtime.handleKernelSymbol(symbol());

  auto event = dispatch();
  auto decision = runtime.handleDispatch(event);
  EXPECT_EQ(decision, aegis::hsa_intercept::DispatchDecision::proceed);

  auto lastRewrite = runtime.lastRewriteResult();
  ASSERT_TRUE(lastRewrite.has_value());
  EXPECT_EQ(lastRewrite->trace.kernel.entryPc, 0x2000u);
  EXPECT_EQ(lastRewrite->trace.kernel.textRange.start, 0x2000u);
  EXPECT_NE(lastRewrite->trace.kernel.entryPc, symbol().kernelObject);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "rewritten");
}

TEST(AegisbitRuntimeTest, ArtifactSinkReceivesRewriteBundle) {
  MockBackend backend;
  MockCodeObjectParser parser;
  std::vector<aegisbit_output::ReproducerBundle> bundles;
  aegisbit_runtime::RuntimeCallbacks callbacks;
  callbacks.onArtifact =
      [&bundles](const aegisbit_output::ReproducerBundle &bundle) {
        bundles.push_back(bundle);
      };
  aegisbit_runtime::Runtime runtime({}, &backend, &parser, nullptr, callbacks);
  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes.assign(128, 0xCC);
  object.loadBase = 0x1000;
  object.loadSize = object.bytes.size();
  runtime.handleCodeObject(object);
  runtime.handleKernelSymbol(symbol());

  auto event = dispatch();
  auto decision = runtime.handleDispatch(event);
  EXPECT_EQ(decision, aegis::hsa_intercept::DispatchDecision::proceed);

  ASSERT_EQ(bundles.size(), 1u);
  ASSERT_GE(bundles[0].files.size(), 5u);
  EXPECT_EQ(bundles[0].files[0].name, "original_code_object.bin");
  EXPECT_EQ(bundles[0].files[1].name, "patched_code_object.bin");
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "rewritten-artifact");
  EXPECT_EQ(runtime.stats().artifactBundles, 1u);
}

TEST(AegisbitRuntimeTest, LiveNoopPatchLoadsAndRedirectsDispatch) {
  MockBackend backend;
  MockCodeObjectParser parser;
  FakeKernelLoader loader;
  aegisbit_runtime::RuntimeOptions options;
  options.liveNoopPatch = true;
  aegisbit_runtime::Runtime runtime(options, &backend, &parser, &loader);
  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes.assign(128, 0xCC);
  object.loadBase = 0x1000;
  object.loadSize = object.bytes.size();
  runtime.handleCodeObject(object);
  runtime.handleKernelSymbol(symbol());

  auto first = dispatch();
  auto firstDecision = runtime.handleDispatch(first);
  EXPECT_EQ(firstDecision, aegis::hsa_intercept::DispatchDecision::proceed);
  EXPECT_EQ(first.packet.kernelObject, 0xDEFu);
  EXPECT_EQ(loader.loadCount, 1u);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "loaded-patched-kernel");

  auto second = dispatch();
  auto secondDecision = runtime.handleDispatch(second);
  EXPECT_EQ(secondDecision, aegis::hsa_intercept::DispatchDecision::proceed);
  EXPECT_EQ(second.packet.kernelObject, 0xDEFu);
  EXPECT_EQ(loader.loadCount, 1u);
  EXPECT_EQ(runtime.dispatchTraces()[1].status, "redirected");
  EXPECT_EQ(runtime.stats().loadedPatchedKernels, 1u);
  EXPECT_EQ(runtime.stats().redirectedDispatches, 2u);
}

TEST(AegisbitRuntimeTest, LiveNoopPatchHardFailsWithoutLoader) {
  MockBackend backend;
  MockCodeObjectParser parser;
  aegisbit_runtime::RuntimeOptions options;
  options.liveNoopPatch = true;
  aegisbit_runtime::Runtime runtime(options, &backend, &parser);
  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes.assign(128, 0xCC);
  object.loadBase = 0x1000;
  object.loadSize = object.bytes.size();
  runtime.handleCodeObject(object);
  runtime.handleKernelSymbol(symbol());

  auto event = dispatch();
  auto decision = runtime.handleDispatch(event);
  EXPECT_EQ(decision, aegis::hsa_intercept::DispatchDecision::skip);
  EXPECT_EQ(event.packet.kernelObject, 0xABCu);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "load-error");
  EXPECT_EQ(runtime.stats().hardFailedDispatches, 1u);
}
