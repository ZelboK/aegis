//===-- Runtime.cpp - new_aegis runtime composition ------------*- C++ -*-===//

#include "aegisbit_runtime/Runtime.h"

#include <sstream>
#include <utility>

namespace aegisbit_runtime {
namespace {

amdgpu_rewrite_core::RewriteRequest buildRewriteRequest(
    const aegis::hsa_intercept::CapturedCodeObject &codeObject,
    const aegis::hsa_intercept::CapturedKernelSymbol &symbol,
    uint64_t dispatchId,
    const amdgpu_code_object::CodeObjectParser *codeObjectParser) {
  amdgpu_rewrite_core::RewriteRequest request;
  std::ostringstream rewriteId;
  rewriteId << symbol.kernelName << "-dispatch-" << dispatchId;
  request.rewriteId = rewriteId.str();
  request.kernelName = symbol.kernelName;
  request.arch = "unknown";
  request.kernelObject = symbol.kernelObject;
  request.entryPc = codeObject.loadBase;
  request.textBase = codeObject.loadBase;
  request.textOffset = 0;
  request.textSize =
      codeObject.loadSize == 0 ? codeObject.bytes.size() : codeObject.loadSize;
  request.codeObjectBytes = codeObject.bytes;
  request.codeObjectParser = codeObjectParser;
  return request;
}

} // namespace

Status::Status(StatusCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::success() { return {}; }

bool Status::ok() const { return code_ == StatusCode::ok; }

StatusCode Status::code() const { return code_; }

const std::string &Status::message() const { return message_; }

Runtime::Runtime(RuntimeOptions options,
                 const amdgpu_instr_backend::InstructionBackend *backend,
                 const amdgpu_code_object::CodeObjectParser *codeObjectParser,
                 hsa_kernel_loader::KernelLoader *kernelLoader,
                 RuntimeCallbacks callbacks)
    : options_(std::move(options)), backend_(backend),
      codeObjectParser_(codeObjectParser), kernelLoader_(kernelLoader),
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

void Runtime::setCodeObjectParser(
    const amdgpu_code_object::CodeObjectParser *codeObjectParser) {
  std::lock_guard<std::mutex> lock(mutex_);
  codeObjectParser_ = codeObjectParser;
}

void Runtime::setKernelLoader(hsa_kernel_loader::KernelLoader *kernelLoader) {
  std::lock_guard<std::mutex> lock(mutex_);
  kernelLoader_ = kernelLoader;
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

  auto hardFail = [&]() {
    stats_.hardFailedDispatches++;
    dispatchTraces_.push_back(trace);
    return aegis::hsa_intercept::DispatchDecision::skip;
  };

  if (options_.liveNoopPatch) {
    auto loadedIt = loadedKernels_.find(event.originalKernelObject);
    if (loadedIt != loadedKernels_.end() &&
        loadedIt->second.patchedKernelObject != 0) {
      event.packet.kernelObject = loadedIt->second.patchedKernelObject;
      trace.rewriteId = loadedIt->second.rewriteId;
      trace.patchedKernelObject = event.packet.kernelObject;
      trace.status = "redirected";
      stats_.redirectedDispatches++;
      dispatchTraces_.push_back(trace);
      return aegis::hsa_intercept::DispatchDecision::proceed;
    }
  }

  auto symbolIt = symbols_.find(event.originalKernelObject);
  if (backend_ && symbolIt != symbols_.end()) {
    const auto &symbol = symbolIt->second;
    auto codeObjectIt = codeObjects_.find(symbol.codeObjectId);
    if (codeObjectIt != codeObjects_.end()) {
      amdgpu_rewrite_core::RewriteRequest request =
          buildRewriteRequest(codeObjectIt->second, symbol, trace.dispatchId,
                              codeObjectParser_);

      amdgpu_rewrite_core::Rewriter rewriter(*backend_);
      auto rewrite = rewriter.rewrite(request, options_.rewriteOptions);
      stats_.rewriteAttempts++;
      if (rewrite) {
        lastRewrite_ = rewrite.takeValue();
        trace.rewriteId = lastRewrite_->trace.rewriteId;
        // TODO: Should this be the way it is? Live mode should hard-fail
        // rewrite errors instead of only recording a status.
        if (lastRewrite_->hasErrors()) {
          trace.status = "rewrite-error";
          if (options_.liveNoopPatch) {
            return hardFail();
          }
        } else if (lastRewrite_->trace.plan.selectedSites.empty()) {
          trace.status = "no-sites";
          trace.note = "rewrite selected no patchable sites";
          if (options_.liveNoopPatch) {
            return hardFail();
          }
        } else {
          trace.status = callbacks_.onArtifact ? "rewritten-artifact" : "rewritten";
        }
        if (callbacks_.onArtifact) {
          callbacks_.onArtifact(aegisbit_output::buildReproducerBundle(
              *lastRewrite_, {trace}, codeObjectIt->second.bytes));
          stats_.artifactBundles++;
        }
        if (options_.liveNoopPatch) {
          if (!kernelLoader_) {
            trace.status = "load-error";
            trace.note = "live NoopPatch requested without a kernel loader";
            return hardFail();
          }

          hsa_kernel_loader::LoadRequest loadRequest;
          loadRequest.codeObjectBytes = lastRewrite_->patched.bytes;
          loadRequest.kernelName = symbol.kernelName;
          loadRequest.originalKernargSize = symbol.kernargSegmentSize;
          auto loaded = kernelLoader_->loadKernel(loadRequest);
          if (!loaded) {
            trace.status = "load-error";
            trace.note = loaded.error();
            loadedKernels_[event.originalKernelObject] =
                {0, trace.rewriteId, trace.status, trace.note};
            return hardFail();
          }

          auto loadedKernel = loaded.takeValue();
          loadedKernels_[event.originalKernelObject] =
              {loadedKernel.kernelObject, trace.rewriteId,
               "loaded-patched-kernel", {}};
          stats_.loadedPatchedKernels++;
          event.packet.kernelObject = loadedKernel.kernelObject;
          trace.patchedKernelObject = event.packet.kernelObject;
          trace.status = "loaded-patched-kernel";
          stats_.redirectedDispatches++;
        }
      } else {
        // TODO: Should this be the way it is? Live mode should hard-fail
        // rewrite failures instead of continuing the original dispatch.
        trace.status = "rewrite-failed";
        trace.note = rewrite.error();
        if (options_.liveNoopPatch) {
          return hardFail();
        }
      }
    } else {
      // TODO: Should this be the way it is? Missing code objects should be
      // hard failures for live instrumentation.
      trace.status = "code-object-missing";
      if (options_.liveNoopPatch) {
        return hardFail();
      }
    }
  } else if (!backend_) {
    // TODO: Should this be the way it is? Missing backends should be hard
    // failures when instrumentation is required.
    trace.status = "no-backend";
    if (options_.liveNoopPatch) {
      return hardFail();
    }
  } else {
    // TODO: Should this be the way it is? Unknown kernel symbols should be
    // hard failures for live instrumentation.
    trace.status = "kernel-symbol-missing";
    if (options_.liveNoopPatch) {
      return hardFail();
    }
  }

  dispatchTraces_.push_back(trace);
  // TODO: Should this be the way it is? Pass-through is correct for artifact
  // mode, but live instrumentation needs an explicit hard-failure policy.
  return aegis::hsa_intercept::DispatchDecision::proceed;
}

} // namespace aegisbit_runtime
