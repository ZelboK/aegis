//===-- aegisbit_runtime/Runtime.h -----------------------------*- C++ -*-===//

#ifndef AEGISBIT_RUNTIME_RUNTIME_H
#define AEGISBIT_RUNTIME_RUNTIME_H

#include "aegis/hsa_intercept/HsaIntercept.h"
#include "aegisbit_output/Output.h"
#include "amdgpu_instr_backend/Backend.h"
#include "amdgpu_rewrite_core/RewriteCore.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aegisbit_runtime {

enum class StatusCode {
  ok,
  interceptInstallFailed,
};

class Status {
public:
  Status() = default;
  Status(StatusCode code, std::string message = {});

  [[nodiscard]] static Status success();
  [[nodiscard]] bool ok() const;
  [[nodiscard]] StatusCode code() const;
  [[nodiscard]] const std::string &message() const;

private:
  StatusCode code_ = StatusCode::ok;
  std::string message_;
};

struct RuntimeOptions {
  bool enabled = true;
  amdgpu_rewrite_core::RewriteOptions rewriteOptions;
};

struct RuntimeStats {
  uint64_t capturedCodeObjects = 0;
  uint64_t capturedKernelSymbols = 0;
  uint64_t observedDispatches = 0;
  uint64_t rewriteAttempts = 0;
};

struct RuntimeCallbacks {
  std::function<void(const std::string &)> log;
};

class Runtime {
public:
  Runtime(RuntimeOptions options = {},
          const amdgpu_instr_backend::InstructionBackend *backend = nullptr,
          RuntimeCallbacks callbacks = {});

  [[nodiscard]] Status install();
  void uninstall();

  [[nodiscard]] RuntimeStats stats() const;
  [[nodiscard]] std::vector<aegisbit_output::DispatchTrace>
  dispatchTraces() const;
  [[nodiscard]] std::optional<amdgpu_rewrite_core::RewriteResult>
  lastRewriteResult() const;

  void setInstructionBackend(
      const amdgpu_instr_backend::InstructionBackend *backend);

  void handleCodeObject(const aegis::hsa_intercept::CapturedCodeObject &object);
  void handleKernelSymbol(const aegis::hsa_intercept::CapturedKernelSymbol &symbol);
  aegis::hsa_intercept::DispatchDecision
  handleDispatch(aegis::hsa_intercept::DispatchEvent &event);

private:
  RuntimeOptions options_;
  const amdgpu_instr_backend::InstructionBackend *backend_ = nullptr;
  RuntimeCallbacks callbacks_;

  mutable std::mutex mutex_;
  RuntimeStats stats_;
  uint64_t nextDispatchId_ = 1;
  std::map<uint64_t, aegis::hsa_intercept::CapturedCodeObject> codeObjects_;
  std::map<uint64_t, aegis::hsa_intercept::CapturedKernelSymbol> symbols_;
  std::vector<aegisbit_output::DispatchTrace> dispatchTraces_;
  std::optional<amdgpu_rewrite_core::RewriteResult> lastRewrite_;
};

} // namespace aegisbit_runtime

#endif // AEGISBIT_RUNTIME_RUNTIME_H
