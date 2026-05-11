//===-- PreloadRuntime.cpp - libaegis preload composition -------*- C++ -*-===//

#include "aegisbit_runtime/Runtime.h"
#include "hsa_kernel_loader/HsaKernelLoader.h"

#if defined(NEW_AEGIS_HAS_LLVM_BACKEND)
#include "amdgpu_code_object_llvm/LlvmCodeObjectParser.h"
#include "amdgpu_instr_backend_llvm/LlvmBackend.h"
#endif

#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace {

std::unique_ptr<amdgpu_instr_backend::InstructionBackend> instructionBackend;
std::unique_ptr<amdgpu_code_object::CodeObjectParser> codeObjectParser;
std::unique_ptr<hsa_kernel_loader::KernelLoader> kernelLoader;
std::unique_ptr<hsa_kernel_loader::ProfilingBufferAllocator> profilingAllocator;
std::unique_ptr<hsa_kernel_loader::DispatchCompletionObserver>
    completionObserver;
std::unique_ptr<hsa_kernel_loader::AgentResourceProvider> agentResources;
std::unique_ptr<aegisbit_runtime::Runtime> runtime;
uint64_t artifactSequence = 0;

void logMessage(const std::string &message) {
  std::cerr << "aegis: " << message << '\n';
}

void debugLog(const char *location, const char *message,
              const char *hypothesisId, const std::string &data) {
  std::ofstream out("/home/djavady/aegis_three/.cursor/debug-748d53.log",
                    std::ios::app);
  if (!out) {
    return;
  }
  const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  out << "{\"sessionId\":\"748d53\",\"runId\":\"initial\",\"hypothesisId\":\""
      << hypothesisId << "\",\"location\":\"" << location
      << "\",\"message\":\"" << message << "\",\"data\":" << data
      << ",\"timestamp\":" << timestamp << "}\n";
}

#if defined(NEW_AEGIS_HAS_LLVM_BACKEND)
class LazyLlvmInstructionBackend final
    : public amdgpu_instr_backend::InstructionBackend {
public:
  explicit LazyLlvmInstructionBackend(
      amdgpu_instr_backend_llvm::LlvmBackendOptions options = {})
      : options_(std::move(options)) {}

  const amdgpu_instr_backend::OpcodeInfo &opcodes() const override {
    auto backend = ensureBackend();
    if (!backend) {
      logMessage("lazy LLVM backend init failed: " + backend.error());
      return emptyOpcodes_;
    }
    return backend.value()->opcodes();
  }

  amdgpu_instr_backend::Result<amdgpu_instr_backend::Instruction>
  decode(amdgpu_instr_backend::ByteView bytes, uint64_t address) const override {
    auto backend = ensureBackend();
    if (!backend) {
      return amdgpu_instr_backend::Result<
          amdgpu_instr_backend::Instruction>::failure(backend.error());
    }
    return backend.value()->decode(bytes, address);
  }

  amdgpu_instr_backend::Result<std::vector<amdgpu_instr_backend::Instruction>>
  decodeAll(amdgpu_instr_backend::ByteView bytes,
            uint64_t baseAddress) const override {
    auto backend = ensureBackend();
    if (!backend) {
      return amdgpu_instr_backend::Result<
          std::vector<amdgpu_instr_backend::Instruction>>::failure(
          backend.error());
    }
    return backend.value()->decodeAll(bytes, baseAddress);
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>>
  encode(const amdgpu_instr_backend::Instruction &inst) const override {
    auto backend = ensureBackend();
    if (!backend) {
      return amdgpu_instr_backend::Result<std::vector<uint8_t>>::failure(
          backend.error());
    }
    return backend.value()->encode(inst);
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>>
  encodeSBranch(int16_t dwordOffset) const override {
    auto backend = ensureBackend();
    if (!backend) {
      return amdgpu_instr_backend::Result<std::vector<uint8_t>>::failure(
          backend.error());
    }
    return backend.value()->encodeSBranch(dwordOffset);
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>>
  encodeNop() const override {
    auto backend = ensureBackend();
    if (!backend) {
      return amdgpu_instr_backend::Result<std::vector<uint8_t>>::failure(
          backend.error());
    }
    return backend.value()->encodeNop();
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>> encodeCountingRecordWrite(
      const amdgpu_instr_backend::CountingRecordWrite &request) const override {
    auto backend = ensureBackend();
    if (!backend) {
      return amdgpu_instr_backend::Result<std::vector<uint8_t>>::failure(
          backend.error());
    }
    return backend.value()->encodeCountingRecordWrite(request);
  }

  amdgpu_instr_backend::Result<uint64_t>
  branchTarget(const amdgpu_instr_backend::Instruction &inst,
               uint64_t currentPc) const override {
    auto backend = ensureBackend();
    if (!backend) {
      return amdgpu_instr_backend::Result<uint64_t>::failure(backend.error());
    }
    return backend.value()->branchTarget(inst, currentPc);
  }

  const amdgpu_instr_backend::SgprPairInfo *
  getSgprPairInfo(unsigned pairReg) const override {
    auto backend = ensureBackend();
    if (!backend) {
      logMessage("lazy LLVM backend init failed: " + backend.error());
      return nullptr;
    }
    return backend.value()->getSgprPairInfo(pairReg);
  }

private:
  amdgpu_instr_backend::Result<
      const amdgpu_instr_backend::InstructionBackend *>
  ensureBackend() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backend_) {
      return amdgpu_instr_backend::Result<
          const amdgpu_instr_backend::InstructionBackend *>::success(
          backend_.get());
    }
    if (!initError_.empty()) {
      return amdgpu_instr_backend::Result<
          const amdgpu_instr_backend::InstructionBackend *>::failure(
          initError_);
    }

    auto backendOrErr = amdgpu_instr_backend_llvm::createLlvmBackend(options_);
    if (!backendOrErr) {
      initError_ = backendOrErr.error();
      return amdgpu_instr_backend::Result<
          const amdgpu_instr_backend::InstructionBackend *>::failure(
          initError_);
    }

    backend_ = backendOrErr.takeValue();
    return amdgpu_instr_backend::Result<
        const amdgpu_instr_backend::InstructionBackend *>::success(
        backend_.get());
  }

  amdgpu_instr_backend_llvm::LlvmBackendOptions options_;
  mutable std::mutex mutex_;
  mutable std::unique_ptr<amdgpu_instr_backend::InstructionBackend> backend_;
  mutable std::string initError_;
  amdgpu_instr_backend::OpcodeInfo emptyOpcodes_;
};
#endif

bool envEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value && (std::string(value) == "1" || std::string(value) == "true" ||
                   std::string(value) == "TRUE" || std::string(value) == "yes");
}

uint64_t envUInt64(const char *name) {
  const char *value = std::getenv(name);
  if (!value || std::string(value).empty()) {
    return 0;
  }
  return std::strtoull(value, nullptr, 0);
}

std::string envString(const char *name) {
  const char *value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

amdgpu_rewrite_core::InstrumentationLevel requestedInstrumentation() {
  const char *value = std::getenv("AEGIS_INSTRUMENTATION");
  if (!value) {
    return amdgpu_rewrite_core::InstrumentationLevel::noopPatch;
  }
  const std::string mode(value);
  if (mode == "dry" || mode == "dryPayload") {
    return amdgpu_rewrite_core::InstrumentationLevel::dryPayload;
  }
  if (mode == "counting" || mode == "countingPayload") {
    return amdgpu_rewrite_core::InstrumentationLevel::countingPayload;
  }
  return amdgpu_rewrite_core::InstrumentationLevel::noopPatch;
}

void writeArtifactBundle(const aegisbit_output::ReproducerBundle &bundle) {
  const char *dir = std::getenv("AEGIS_ARTIFACT_DIR");
  if (!dir || std::string(dir).empty()) {
    return;
  }
  const uint64_t seq = ++artifactSequence;
  for (const auto &file : bundle.files) {
    std::ostringstream path;
    path << dir << "/aegis_" << seq << "_" << file.name;
    std::ofstream out(path.str(), std::ios::binary);
    if (!out) {
      logMessage("failed to open artifact file: " + path.str());
      continue;
    }
    out.write(reinterpret_cast<const char *>(file.bytes.data()),
              static_cast<std::streamsize>(file.bytes.size()));
  }
}

} // namespace

extern "C" void aegis_preload_initialize() {
#if defined(NEW_AEGIS_HAS_LLVM_BACKEND)
  instructionBackend = std::make_unique<LazyLlvmInstructionBackend>();
  codeObjectParser = amdgpu_code_object_llvm::createLlvmCodeObjectParser();
#endif

  aegisbit_runtime::RuntimeCallbacks callbacks;
  callbacks.log = logMessage;
  callbacks.onArtifact = writeArtifactBundle;

  aegisbit_runtime::RuntimeOptions options;
  options.liveNoopPatch = envEnabled("AEGIS_LIVE_NOOPATCH");
  options.kernelNameFilter = envString("AEGIS_KERNEL_FILTER");
  options.countingBufferAddress = envUInt64("AEGIS_COUNTING_BUFFER_ADDR");
  options.countingBufferSize = envUInt64("AEGIS_COUNTING_BUFFER_SIZE");
  options.rewriteOptions.instrumentation = requestedInstrumentation();
  // #region agent log
  debugLog("PreloadRuntime.cpp:aegis_preload_initialize:options",
           "preload runtime options resolved", "H1,H5",
           std::string("{\"liveNoopPatch\":") +
               (options.liveNoopPatch ? "true" : "false") +
               ",\"instrumentation\":" +
               std::to_string(static_cast<int>(
                   options.rewriteOptions.instrumentation)) +
               "}");
  // #endregion

  if (options.liveNoopPatch && !instructionBackend) {
    logMessage("live instrumentation requested without an instruction backend");
  }
  if (options.liveNoopPatch && !codeObjectParser) {
    logMessage("live instrumentation requested without a code object parser");
  }

  if (options.liveNoopPatch) {
    agentResources = hsa_kernel_loader::createHsaAgentResourceProvider();
  }

  runtime = std::make_unique<aegisbit_runtime::Runtime>(
      options, instructionBackend.get(), codeObjectParser.get(), kernelLoader.get(),
      std::move(callbacks), profilingAllocator.get(), completionObserver.get(),
      agentResources.get());

  auto status = runtime->install();
  if (!status.ok()) {
    logMessage(status.message());
    if (options.liveNoopPatch) {
      std::abort();
    }
  }
}

extern "C" void aegis_preload_finalize() {
  if (runtime) {
    runtime->uninstall();
    runtime.reset();
  }
  instructionBackend.reset();
  codeObjectParser.reset();
  kernelLoader.reset();
  profilingAllocator.reset();
  completionObserver.reset();
  agentResources.reset();
}

__attribute__((constructor)) static void aegisPreloadConstructor() {
  aegis_preload_initialize();
}

__attribute__((destructor)) static void aegisPreloadDestructor() {
  aegis_preload_finalize();
}
