//===-- PreloadRuntime.cpp - libaegis preload composition -------*- C++ -*-===//

#include "aegisbit_runtime/Runtime.h"

#if defined(NEW_AEGIS_HAS_LLVM_BACKEND)
#include "amdgpu_instr_backend_llvm/LlvmBackend.h"
#endif

#include <iostream>
#include <memory>
#include <utility>

namespace {

std::unique_ptr<amdgpu_instr_backend::InstructionBackend> instructionBackend;
std::unique_ptr<aegisbit_runtime::Runtime> runtime;

void logMessage(const std::string& message) {
  std::cerr << "aegis: " << message << '\n';
}

} // namespace

extern "C" void aegis_preload_initialize() {
#if defined(NEW_AEGIS_HAS_LLVM_BACKEND)
  auto backendOrErr = amdgpu_instr_backend_llvm::createLlvmBackend();
  if (backendOrErr) {
    instructionBackend = backendOrErr.takeValue();
  } else {
    logMessage(backendOrErr.error());
  }
#endif

  aegisbit_runtime::RuntimeCallbacks callbacks;
  callbacks.log = logMessage;

  aegisbit_runtime::RuntimeOptions options;
  options.rewriteOptions.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::noopPatch;

  runtime = std::make_unique<aegisbit_runtime::Runtime>(
      options, instructionBackend.get(), std::move(callbacks));

  auto status = runtime->install();
  if (!status.ok()) {
    logMessage(status.message());
  }
}

extern "C" void aegis_preload_finalize() {
  if (runtime) {
    runtime->uninstall();
    runtime.reset();
  }
  instructionBackend.reset();
}

__attribute__((constructor)) static void aegisPreloadConstructor() {
  aegis_preload_initialize();
}

__attribute__((destructor)) static void aegisPreloadDestructor() {
  aegis_preload_finalize();
}
