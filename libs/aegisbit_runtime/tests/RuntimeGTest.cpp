//===-- RuntimeGTest.cpp - runtime composition tests ----------*- C++ -*-===//

#include "aegisbit_runtime/Runtime.h"
#include "amdgpu_rewrite_core/CountingPayloadAbi.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

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

  amdgpu_instr_backend::Result<std::vector<uint8_t>> encodeNop() const override {
    return amdgpu_instr_backend::Result<std::vector<uint8_t>>::success(
        {0x00, 0x00, 0x80, 0xBF});
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>>
  encodeCountingRecordWrite(
      const amdgpu_instr_backend::CountingRecordWrite &) const override {
    return encodeNop();
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
  bool decodeMemorySite_ = false;
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
    kernel.textSection.address = 0x2000;
    kernel.textSection.fileOffset = 16;
    kernel.textSection.size = 64;
    amdgpu_code_object::DescriptorFacts descriptor;
    descriptor.present = true;
    descriptor.fileOffset = 96;
    descriptor.size = 64;
    descriptor.computePgmRsrc1 = 1;
    descriptor.vgprCount = 8;
    descriptor.sgprCount = 16;
    descriptor.vgprGranularity = 4;
    kernel.descriptor = descriptor;
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

class FakeProfilingBufferAllocator final
    : public hsa_kernel_loader::ProfilingBufferAllocator {
public:
  amdgpu_instr_backend::Result<hsa_kernel_loader::ProfilingBuffer>
  allocate(uint64_t size) override {
    allocateCount++;
    storage.assign(size / sizeof(uint64_t), 0);
    if (storage.empty()) {
      storage.assign(1, 0);
    }
    hsa_kernel_loader::ProfilingBuffer buffer;
    buffer.address = reinterpret_cast<uint64_t>(storage.data());
    buffer.size = static_cast<uint64_t>(storage.size() * sizeof(uint64_t));
    return amdgpu_instr_backend::Result<hsa_kernel_loader::ProfilingBuffer>::
        success(buffer);
  }

  void release(hsa_kernel_loader::ProfilingBuffer) override { releaseCount++; }

  uint32_t allocateCount = 0;
  uint32_t releaseCount = 0;
  std::vector<uint64_t> storage;
};

class FakeCompletionObserver final
    : public hsa_kernel_loader::DispatchCompletionObserver {
public:
  amdgpu_instr_backend::Result<bool> waitForCompletion(
      const hsa_kernel_loader::DispatchCompletionRequest &request) override {
    waitCount++;
    lastSignal = request.signalHandle;
    if (!error.empty()) {
      return amdgpu_instr_backend::Result<bool>::failure(error);
    }
    return amdgpu_instr_backend::Result<bool>::success(true);
  }

  uint32_t waitCount = 0;
  uint64_t lastSignal = 0;
  std::string error;
};

void writeCountingRecord(FakeProfilingBufferAllocator &allocator,
                         uint64_t hitCount) {
  amdgpu_rewrite_core::CountingPayloadRecordV1 record;
  record.magic = amdgpu_rewrite_core::CountingPayloadRecordV1::magicValue;
  record.version = amdgpu_rewrite_core::CountingPayloadRecordV1::versionValue;
  record.siteId = 1;
  record.hitCount = hitCount;
  std::memcpy(allocator.storage.data(), &record, sizeof(record));
}

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
  event.completionSignal = 0xFEED;
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

TEST(AegisbitRuntimeTest, LiveInstrumentationHardFailsWithoutBackend) {
  aegisbit_runtime::RuntimeOptions options;
  options.liveNoopPatch = true;
  aegisbit_runtime::Runtime runtime(options);
  runtime.handleKernelSymbol(symbol());
  auto event = dispatch();

  auto decision = runtime.handleDispatch(event);
  EXPECT_EQ(decision, aegis::hsa_intercept::DispatchDecision::skip);
  ASSERT_EQ(runtime.dispatchTraces().size(), 1u);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "no-backend");
  EXPECT_EQ(runtime.stats().hardFailedDispatches, 1u);
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
  EXPECT_TRUE(lastRewrite->trace.plan.selectedSites.empty());
  EXPECT_TRUE(lastRewrite->trace.patches.empty());
  ASSERT_EQ(runtime.dispatchTraces().size(), 1u);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "rewritten");
}

TEST(AegisbitRuntimeTest, DispatchUsesParsedKernelMetadataWhenParserExists) {
  MockBackend backend;
  MockCodeObjectParser parser;
  aegisbit_runtime::Runtime runtime({}, &backend, &parser);
  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes.assign(160, 0xCC);
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
  object.bytes.assign(160, 0xCC);
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
  EXPECT_EQ(loader.lastRequest.codeObjectBytes, object.bytes);
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

TEST(AegisbitRuntimeTest, LiveNoopPatchHardFailsWithoutParser) {
  MockBackend backend;
  FakeKernelLoader loader;
  aegisbit_runtime::RuntimeOptions options;
  options.liveNoopPatch = true;
  aegisbit_runtime::Runtime runtime(options, &backend, nullptr, &loader);
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
  EXPECT_EQ(loader.loadCount, 0u);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "no-code-object-parser");
  EXPECT_EQ(runtime.stats().hardFailedDispatches, 1u);
}

TEST(AegisbitRuntimeTest, LiveInstrumentationHardFailsWithoutCodeObject) {
  MockBackend backend;
  MockCodeObjectParser parser;
  FakeKernelLoader loader;
  aegisbit_runtime::RuntimeOptions options;
  options.liveNoopPatch = true;
  aegisbit_runtime::Runtime runtime(options, &backend, &parser, &loader);
  runtime.handleKernelSymbol(symbol());

  auto event = dispatch();
  auto decision = runtime.handleDispatch(event);
  EXPECT_EQ(decision, aegis::hsa_intercept::DispatchDecision::skip);
  ASSERT_EQ(runtime.dispatchTraces().size(), 1u);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "code-object-missing");
  EXPECT_EQ(runtime.stats().hardFailedDispatches, 1u);
}

TEST(AegisbitRuntimeTest, LiveInstrumentationHardFailsWithoutKernelSymbol) {
  MockBackend backend;
  MockCodeObjectParser parser;
  FakeKernelLoader loader;
  aegisbit_runtime::RuntimeOptions options;
  options.liveNoopPatch = true;
  options.kernelNameFilter = "kernel";
  aegisbit_runtime::Runtime runtime(options, &backend, &parser, &loader);
  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes.assign(160, 0xCC);
  object.loadBase = 0x1000;
  object.loadSize = object.bytes.size();
  runtime.handleCodeObject(object);

  auto event = dispatch();
  auto decision = runtime.handleDispatch(event);
  EXPECT_EQ(decision, aegis::hsa_intercept::DispatchDecision::skip);
  ASSERT_EQ(runtime.dispatchTraces().size(), 1u);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "kernel-symbol-missing");
  EXPECT_EQ(runtime.stats().hardFailedDispatches, 1u);
}

TEST(AegisbitRuntimeTest, LiveDryPayloadUsesSharedLoaderPath) {
  MockBackend backend(/*decodeMemorySite=*/true);
  MockCodeObjectParser parser;
  FakeKernelLoader loader;
  aegisbit_runtime::RuntimeOptions options;
  options.liveNoopPatch = true;
  options.rewriteOptions.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::dryPayload;
  aegisbit_runtime::Runtime runtime(options, &backend, &parser, &loader);
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
  EXPECT_EQ(event.packet.kernelObject, 0xDEFu);
  ASSERT_TRUE(runtime.lastRewriteResult().has_value());
  EXPECT_EQ(runtime.lastRewriteResult()->trace.plan.instrumentation,
            amdgpu_rewrite_core::InstrumentationLevel::dryPayload);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "loaded-patched-kernel");
}

TEST(AegisbitRuntimeTest, LiveCountingPayloadRecordsReadback) {
  MockBackend backend(/*decodeMemorySite=*/true);
  MockCodeObjectParser parser;
  FakeKernelLoader loader;
  FakeProfilingBufferAllocator allocator;
  FakeCompletionObserver completion;
  aegisbit_runtime::RuntimeOptions options;
  options.liveNoopPatch = true;
  options.rewriteOptions.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::countingPayload;
  aegisbit_runtime::Runtime runtime(options, &backend, &parser, &loader, {},
                                    &allocator, &completion);
  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes.assign(160, 0xCC);
  object.loadBase = 0x1000;
  object.loadSize = object.bytes.size();
  runtime.handleCodeObject(object);
  runtime.handleKernelSymbol(symbol());

  auto first = dispatch();
  auto firstDecision = runtime.handleDispatch(first);
  EXPECT_EQ(firstDecision, aegis::hsa_intercept::DispatchDecision::proceed);
  ASSERT_EQ(runtime.dispatchTraces().size(), 1u);
  EXPECT_EQ(runtime.dispatchTraces()[0].profilingRecordCount, 0u);
  writeCountingRecord(allocator, 1);
  runtime.handleDispatchSubmitted(first);
  EXPECT_EQ(runtime.dispatchTraces()[0].profilingRecordCount, 1u);
  ASSERT_EQ(runtime.dispatchTraces()[0].profilingRecords.size(), 1u);
  EXPECT_EQ(runtime.dispatchTraces()[0].profilingRecords[0].abi,
            "CountingPayloadRecordV1");
  EXPECT_EQ(runtime.stats().profilingRecords, 1u);
  EXPECT_EQ(allocator.allocateCount, 1u);
  EXPECT_EQ(completion.waitCount, 1u);

  auto second = dispatch();
  auto secondDecision = runtime.handleDispatch(second);
  EXPECT_EQ(secondDecision, aegis::hsa_intercept::DispatchDecision::proceed);
  ASSERT_EQ(runtime.dispatchTraces().size(), 2u);
  EXPECT_EQ(runtime.dispatchTraces()[1].status, "redirected");
  writeCountingRecord(allocator, 2);
  runtime.handleDispatchSubmitted(second);
  EXPECT_EQ(runtime.dispatchTraces()[1].profilingRecordCount, 2u);
  EXPECT_EQ(runtime.stats().profilingRecords, 3u);
  EXPECT_EQ(completion.waitCount, 2u);
}

TEST(AegisbitRuntimeTest, LiveCountingPayloadHardFailsWithoutCompletionSignal) {
  MockBackend backend(/*decodeMemorySite=*/true);
  MockCodeObjectParser parser;
  FakeKernelLoader loader;
  FakeProfilingBufferAllocator allocator;
  FakeCompletionObserver completion;
  aegisbit_runtime::RuntimeOptions options;
  options.liveNoopPatch = true;
  options.rewriteOptions.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::countingPayload;
  aegisbit_runtime::Runtime runtime(options, &backend, &parser, &loader, {},
                                    &allocator, &completion);
  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes.assign(160, 0xCC);
  object.loadBase = 0x1000;
  object.loadSize = object.bytes.size();
  runtime.handleCodeObject(object);
  runtime.handleKernelSymbol(symbol());

  auto event = dispatch();
  event.completionSignal = 0;
  auto decision = runtime.handleDispatch(event);
  EXPECT_EQ(decision, aegis::hsa_intercept::DispatchDecision::skip);
  ASSERT_EQ(runtime.dispatchTraces().size(), 1u);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "completion-signal-missing");
  EXPECT_EQ(runtime.stats().hardFailedDispatches, 1u);
}
