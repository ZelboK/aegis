//===-- Runtime.cpp - new_aegis runtime composition ------------*- C++ -*-===//

#include "aegisbit_runtime/Runtime.h"

#include <sstream>
#include <utility>

namespace aegisbit_runtime {

Status::Status(StatusCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::success() { return {}; }

bool Status::ok() const { return code_ == StatusCode::ok; }

StatusCode Status::code() const { return code_; }

const std::string &Status::message() const { return message_; }

Runtime::Runtime(RuntimeOptions options,
                 const amdgpu_instr_backend::InstructionBackend *backend,
                 RuntimeCallbacks callbacks)
    : options_(std::move(options)), backend_(backend),
      callbacks_(std::move(callbacks)) {}

Status Runtime::install() {
  if (!options_.enabled) {
    return Status::success();
  }

  aegis::hsa_intercept::Callbacks callbacks;
  callbacks.log = callbacks_.log;
  callbacks.onCodeObject = [this](
                               const aegis::hsa_intercept::CapturedCodeObject
                                   &object) { handleCodeObject(object); };
  callbacks.onKernelSymbol =
      [this](const aegis::hsa_intercept::CapturedKernelSymbol &symbol) {
        handleKernelSymbol(symbol);
      };
  callbacks.onDispatch = [this](aegis::hsa_intercept::DispatchEvent &event) {
    return handleDispatch(event);
  };

  auto status = aegis::hsa_intercept::install({}, std::move(callbacks));
  if (!status.ok()) {
    return {StatusCode::interceptInstallFailed, status.message()};
  }
  return Status::success();
}

void Runtime::uninstall() { aegis::hsa_intercept::uninstall(); }

RuntimeStats Runtime::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

std::vector<aegisbit_output::DispatchTrace> Runtime::dispatchTraces() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dispatchTraces_;
}

std::optional<amdgpu_rewrite_core::RewriteResult>
Runtime::lastRewriteResult() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lastRewrite_;
}

void Runtime::setInstructionBackend(
    const amdgpu_instr_backend::InstructionBackend *backend) {
  std::lock_guard<std::mutex> lock(mutex_);
  backend_ = backend;
}

void Runtime::handleCodeObject(
    const aegis::hsa_intercept::CapturedCodeObject &object) {
  std::lock_guard<std::mutex> lock(mutex_);
  codeObjects_[object.codeObjectId] = object;
  stats_.capturedCodeObjects++;
}

void Runtime::handleKernelSymbol(
    const aegis::hsa_intercept::CapturedKernelSymbol &symbol) {
  std::lock_guard<std::mutex> lock(mutex_);
  symbols_[symbol.kernelObject] = symbol;
  stats_.capturedKernelSymbols++;
}

aegis::hsa_intercept::DispatchDecision
Runtime::handleDispatch(aegis::hsa_intercept::DispatchEvent &event) {
  std::lock_guard<std::mutex> lock(mutex_);
  aegisbit_output::DispatchTrace trace;
  trace.dispatchId = nextDispatchId_++;
  trace.originalKernelObject = event.originalKernelObject;
  trace.patchedKernelObject = event.packet.kernelObject;
  trace.originalKernargAddress = event.originalKernargAddress;
  trace.patchedKernargAddress = event.packet.kernargAddress;
  trace.status = "observed";

  stats_.observedDispatches++;

  auto symbolIt = symbols_.find(event.originalKernelObject);
  if (backend_ && symbolIt != symbols_.end()) {
    const auto &symbol = symbolIt->second;
    auto codeObjectIt = codeObjects_.find(symbol.codeObjectId);
    if (codeObjectIt != codeObjects_.end()) {
      amdgpu_rewrite_core::RewriteRequest request;
      std::ostringstream rewriteId;
      rewriteId << symbol.kernelName << "-dispatch-" << trace.dispatchId;
      request.rewriteId = rewriteId.str();
      request.kernelName = symbol.kernelName;
      request.kernelObject = symbol.kernelObject;
      request.codeObjectBytes = codeObjectIt->second.bytes;
      request.textBase = 0;
      request.textOffset = 0;

      amdgpu_rewrite_core::Rewriter rewriter(*backend_);
      auto rewrite = rewriter.rewrite(request, options_.rewriteOptions);
      stats_.rewriteAttempts++;
      if (rewrite) {
        lastRewrite_ = rewrite.takeValue();
        trace.rewriteId = lastRewrite_->trace.rewriteId;
        trace.status = lastRewrite_->hasErrors() ? "rewrite-error" : "rewritten";
      } else {
        trace.status = "rewrite-failed";
        trace.note = rewrite.error();
      }
    } else {
      trace.status = "code-object-missing";
    }
  } else if (!backend_) {
    trace.status = "no-backend";
  } else {
    trace.status = "kernel-symbol-missing";
  }

  dispatchTraces_.push_back(trace);
  return aegis::hsa_intercept::DispatchDecision::proceed;
}

} // namespace aegisbit_runtime
