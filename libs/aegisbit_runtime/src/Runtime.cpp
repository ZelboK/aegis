//===-- Runtime.cpp - new_aegis runtime composition ------------*- C++ -*-===//

#include "aegisbit_runtime/Runtime.h"

#include "amdgpu_rewrite_core/CountingPayloadAbi.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <utility>

namespace aegisbit_runtime {
namespace {

constexpr uint64_t profilingBufferAllocationSize = 4096;

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

amdgpu_rewrite_core::RewriteRequest buildRewriteRequest(
    const aegis::hsa_intercept::CapturedCodeObject &codeObject,
    const aegis::hsa_intercept::CapturedKernelSymbol &symbol,
    uint64_t dispatchId,
    const amdgpu_code_object::CodeObjectParser *codeObjectParser,
    uint64_t profilingBufferAddress, uint64_t profilingBufferSize) {
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
  request.profilingBufferAddress = profilingBufferAddress;
  request.profilingBufferSize = profilingBufferSize;
  return request;
}

bool isCountingPayload(amdgpu_rewrite_core::InstrumentationLevel level) {
  return level == amdgpu_rewrite_core::InstrumentationLevel::countingPayload;
}

bool isNoopPatch(amdgpu_rewrite_core::InstrumentationLevel level) {
  return level == amdgpu_rewrite_core::InstrumentationLevel::noopPatch;
}

amdgpu_instr_backend::Result<aegisbit_output::ProfilingRecord>
readCountingRecord(uint64_t bufferAddress, uint64_t bufferSize) {
  if (bufferAddress == 0 ||
      bufferSize < amdgpu_rewrite_core::countingPayloadRecordV1Size) {
    return amdgpu_instr_backend::Result<
        aegisbit_output::ProfilingRecord>::failure(
        "counting readback requires a valid profiling buffer");
  }
  const auto *record =
      reinterpret_cast<const amdgpu_rewrite_core::CountingPayloadRecordV1 *>(
          bufferAddress);
  if (record->magic !=
          amdgpu_rewrite_core::CountingPayloadRecordV1::magicValue ||
      record->version !=
          amdgpu_rewrite_core::CountingPayloadRecordV1::versionValue) {
    return amdgpu_instr_backend::Result<
        aegisbit_output::ProfilingRecord>::failure(
        "counting readback did not find a valid CountingPayloadRecordV1");
  }
  aegisbit_output::ProfilingRecord out;
  out.profileMode = "CountingPayload";
  out.abi = "CountingPayloadRecordV1";
  out.bufferAddress = bufferAddress;
  out.siteId = record->siteId;
  out.hitCount = record->hitCount;
  out.status = "read";
  out.note = "record read after dispatch completion";
  return amdgpu_instr_backend::Result<aegisbit_output::ProfilingRecord>::
      success(std::move(out));
}

amdgpu_instr_backend::Result<bool>
initializeCountingRecord(uint64_t bufferAddress, uint64_t bufferSize,
                         uint64_t siteId) {
  if (bufferAddress == 0 ||
      bufferSize < amdgpu_rewrite_core::countingPayloadRecordV1Size) {
    return amdgpu_instr_backend::Result<bool>::failure(
        "counting initialization requires a valid profiling buffer");
  }
  auto *record = reinterpret_cast<amdgpu_rewrite_core::CountingPayloadRecordV1 *>(
      bufferAddress);
  record->magic = amdgpu_rewrite_core::CountingPayloadRecordV1::magicValue;
  record->version = amdgpu_rewrite_core::CountingPayloadRecordV1::versionValue;
  record->siteId = siteId;
  record->hitCount = 0;
  return amdgpu_instr_backend::Result<bool>::success(true);
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
                 RuntimeCallbacks callbacks,
                 hsa_kernel_loader::ProfilingBufferAllocator *profilingAllocator,
                 hsa_kernel_loader::DispatchCompletionObserver
                     *completionObserver,
                 hsa_kernel_loader::AgentResourceProvider *agentResources)
    : options_(std::move(options)), backend_(backend),
      codeObjectParser_(codeObjectParser), kernelLoader_(kernelLoader),
      profilingAllocator_(profilingAllocator),
      completionObserver_(completionObserver), agentResources_(agentResources),
      callbacks_(std::move(callbacks)) {
  if (isCountingPayload(options_.rewriteOptions.instrumentation) &&
      options_.countingBufferAddress == 0 && profilingAllocator_) {
    auto buffer = profilingAllocator_->allocate(
        amdgpu_rewrite_core::countingPayloadRecordV1Size);
    if (buffer) {
      ownedCountingBuffer_ = buffer.takeValue();
      options_.countingBufferAddress = ownedCountingBuffer_.address;
      options_.countingBufferSize = ownedCountingBuffer_.size;
    } else if (callbacks_.log) {
      callbacks_.log(buffer.error());
    }
  }
}

Runtime::~Runtime() {
  flushPendingCountingDispatches();
  if (profilingAllocator_ && ownedCountingBuffer_.address != 0) {
    profilingAllocator_->release(ownedCountingBuffer_);
  }
  if (agentResources_) {
    for (auto [agent, buffer] : agentCountingBuffers_) {
      if (auto *allocator = agentResources_->profilingAllocatorForAgent(agent)) {
        allocator->release(buffer);
      }
    }
  }
}

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
  callbacks.onDispatchSubmitted =
      [this](const aegis::hsa_intercept::DispatchEvent &event) {
        handleDispatchSubmitted(event);
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
  std::unique_lock<std::mutex> lock(mutex_);
  // #region agent log
  debugLog("Runtime.cpp:Runtime::handleDispatch:entry",
           "runtime dispatch handling entered", "H4",
           std::string("{\"kernelObject\":") +
               std::to_string(event.originalKernelObject) +
               ",\"liveNoopPatch\":" +
               (options_.liveNoopPatch ? "true" : "false") + "}");
  // #endregion
  aegisbit_output::DispatchTrace trace;
  trace.dispatchId = nextDispatchId_++;
  trace.originalKernelObject = event.originalKernelObject;
  trace.patchedKernelObject = event.packet.kernelObject;
  trace.originalKernargAddress = event.originalKernargAddress;
  trace.patchedKernargAddress = event.packet.kernargAddress;
  trace.status = "observed";

  stats_.observedDispatches++;

  auto hardFail = [&]() {
    if (callbacks_.log) {
      callbacks_.log("live instrumentation hard-failed dispatch " +
                     std::to_string(trace.dispatchId) + ": " + trace.status +
                     (trace.note.empty() ? std::string()
                                         : std::string(" (") + trace.note + ")"));
    }
    stats_.hardFailedDispatches++;
    dispatchTraces_.push_back(trace);
    return aegis::hsa_intercept::DispatchDecision::skip;
  };

  hsa_kernel_loader::KernelLoader *kernelLoader = kernelLoader_;
  hsa_kernel_loader::DispatchCompletionObserver *completionObserver =
      completionObserver_;
  uint64_t countingBufferAddress = options_.countingBufferAddress;
  uint64_t countingBufferSize = options_.countingBufferSize;
  bool ownsCompletionSignal = false;
  if (options_.liveNoopPatch && agentResources_) {
    if (event.queueAgent == 0) {
      trace.status = "queue-agent-missing";
      trace.note = "live instrumentation requires dispatch queue agent binding";
      return hardFail();
    }
    kernelLoader = agentResources_->kernelLoaderForAgent(event.queueAgent);
    completionObserver = agentResources_->completionObserver();
    if (!kernelLoader) {
      trace.status = "load-error";
      trace.note = "no kernel loader available for dispatch queue agent";
      return hardFail();
    }
    if (isCountingPayload(options_.rewriteOptions.instrumentation)) {
      auto bufferIt = agentCountingBuffers_.find(event.queueAgent);
      if (bufferIt == agentCountingBuffers_.end()) {
        auto *allocator =
            agentResources_->profilingAllocatorForAgent(event.queueAgent);
        if (!allocator) {
          trace.status = "profiling-buffer-missing";
          trace.note = "no profiling allocator available for dispatch queue agent";
          return hardFail();
        }
        auto buffer = allocator->allocate(profilingBufferAllocationSize);
        if (!buffer) {
          trace.status = "profiling-buffer-missing";
          trace.note = buffer.error();
          return hardFail();
        }
        bufferIt =
            agentCountingBuffers_.emplace(event.queueAgent, buffer.takeValue())
                .first;
      }
      countingBufferAddress = bufferIt->second.address;
      countingBufferSize = bufferIt->second.size;
    }
  }

  auto armCountingReadback = [&]() -> bool {
    if (!isCountingPayload(options_.rewriteOptions.instrumentation)) {
      return true;
    }
    if (!completionObserver) {
      trace.status = "completion-observer-missing";
      trace.note = "CountingPayload requires completion-gated readback";
      return false;
    }
    if (event.completionSignal == 0) {
      auto createdSignal = completionObserver->createSignal(1);
      if (!createdSignal) {
        trace.status = "completion-signal-missing";
        trace.note = createdSignal.error();
        return false;
      }
      event.completionSignal = createdSignal.value();
      event.packet.completionSignal = createdSignal.value();
      ownsCompletionSignal = true;
    }
    if (countingBufferAddress == 0 ||
        countingBufferSize <
            amdgpu_rewrite_core::countingPayloadRecordV1Size) {
      trace.status = "profiling-buffer-missing";
      trace.note = "CountingPayload requires a valid profiling buffer";
      return false;
    }
    auto initialized =
        initializeCountingRecord(countingBufferAddress, countingBufferSize, 1);
    if (!initialized) {
      trace.status = "profiling-buffer-init-failed";
      trace.note = initialized.error();
      return false;
    }
    event.correlationId = trace.dispatchId;
    trace.note = "counting readback pending dispatch completion";
    pendingCountingDispatches_[trace.dispatchId] = {
        countingBufferAddress, countingBufferSize, dispatchTraces_.size(),
        completionObserver, event.completionSignal, ownsCompletionSignal, false};
    return true;
  };

  LoadedKernelCacheKey cacheKey{
      event.originalKernelObject,
      event.queueAgent,
      options_.rewriteOptions.instrumentation,
      options_.rewriteOptions.zeroSgprFlavor,
      countingBufferAddress};
  if (options_.liveNoopPatch) {
    auto loadedIt = loadedKernels_.find(cacheKey);
    if (loadedIt != loadedKernels_.end() &&
        loadedIt->second.patchedKernelObject != 0) {
      event.packet.kernelObject = loadedIt->second.patchedKernelObject;
      trace.rewriteId = loadedIt->second.rewriteId;
      trace.patchedKernelObject = event.packet.kernelObject;
      trace.status = "redirected";
      if (isCountingPayload(loadedIt->second.instrumentation) &&
          !armCountingReadback()) {
        return hardFail();
      }
      stats_.redirectedDispatches++;
      dispatchTraces_.push_back(trace);
      return aegis::hsa_intercept::DispatchDecision::proceed;
    }
  }

  auto symbolIt = symbols_.find(event.originalKernelObject);
  if (backend_ && symbolIt != symbols_.end()) {
    const auto &symbol = symbolIt->second;
    if (!options_.kernelNameFilter.empty() &&
        symbol.kernelName.find(options_.kernelNameFilter) == std::string::npos) {
      trace.status = "filtered";
      trace.note = "kernel did not match live instrumentation filter";
      dispatchTraces_.push_back(trace);
      return aegis::hsa_intercept::DispatchDecision::proceed;
    }

    auto codeObjectIt = codeObjects_.find(symbol.codeObjectId);
    if (codeObjectIt != codeObjects_.end()) {
      if (options_.liveNoopPatch && !codeObjectParser_) {
        trace.status = "no-code-object-parser";
        trace.note = "live instrumentation requires parsed code object metadata";
        return hardFail();
      }

      amdgpu_rewrite_core::RewriteRequest request =
          buildRewriteRequest(codeObjectIt->second, symbol, trace.dispatchId,
                              codeObjectParser_, countingBufferAddress,
                              countingBufferSize);

      amdgpu_rewrite_core::Rewriter rewriter(*backend_);
      auto rewrite = rewriter.rewrite(request, options_.rewriteOptions);
      stats_.rewriteAttempts++;
      if (rewrite) {
        lastRewrite_ = rewrite.takeValue();
        trace.rewriteId = lastRewrite_->trace.rewriteId;
        if (lastRewrite_->hasErrors()) {
          trace.status = "rewrite-error";
          if (options_.liveNoopPatch) {
            return hardFail();
          }
        } else if (lastRewrite_->trace.plan.selectedSites.empty() &&
                   !isNoopPatch(options_.rewriteOptions.instrumentation)) {
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
          if (!kernelLoader) {
            trace.status = "load-error";
            trace.note = "live NoopPatch requested without a kernel loader";
            return hardFail();
          }

          hsa_kernel_loader::LoadRequest loadRequest;
          loadRequest.codeObjectBytes = lastRewrite_->patched.bytes;
          loadRequest.kernelName = symbol.kernelName;
          loadRequest.originalKernargSize = symbol.kernargSegmentSize;
          // #region agent log
          debugLog("Runtime.cpp:Runtime::handleDispatch:before-load",
                   "loading patched kernel with runtime mutex released", "H4",
                   std::string("{\"rewriteId\":\"") + trace.rewriteId +
                       "\",\"kernelName\":\"" + symbol.kernelName + "\"}");
          // #endregion
          lock.unlock();
          auto loaded = kernelLoader->loadKernel(loadRequest);
          lock.lock();
          // #region agent log
          debugLog("Runtime.cpp:Runtime::handleDispatch:after-load",
                   "patched kernel load returned", "H4",
                   std::string("{\"ok\":") + (loaded ? "true" : "false") +
                       "}");
          // #endregion
          if (!loaded) {
            trace.status = "load-error";
            trace.note = loaded.error();
            loadedKernels_[cacheKey] =
                {0, trace.rewriteId, trace.status, trace.note,
                 options_.rewriteOptions.instrumentation};
            return hardFail();
          }

          auto loadedKernel = loaded.takeValue();
          loadedKernels_[cacheKey] =
              {loadedKernel.kernelObject, trace.rewriteId,
               "loaded-patched-kernel", {},
               options_.rewriteOptions.instrumentation};
          stats_.loadedPatchedKernels++;
          event.packet.kernelObject = loadedKernel.kernelObject;
          trace.patchedKernelObject = event.packet.kernelObject;
          trace.status = "loaded-patched-kernel";
          if (isCountingPayload(options_.rewriteOptions.instrumentation) &&
              !armCountingReadback()) {
            return hardFail();
          }
          stats_.redirectedDispatches++;
          dispatchTraces_.push_back(trace);
          return aegis::hsa_intercept::DispatchDecision::proceed;
        }
      } else {
        trace.status = "rewrite-failed";
        trace.note = rewrite.error();
        if (options_.liveNoopPatch) {
          return hardFail();
        }
      }
    } else {
      trace.status = "code-object-missing";
      if (options_.liveNoopPatch) {
        return hardFail();
      }
    }
  } else if (!backend_) {
    trace.status = "no-backend";
    if (options_.liveNoopPatch) {
      return hardFail();
    }
  } else {
    trace.status = "kernel-symbol-missing";
    if (options_.liveNoopPatch) {
      return hardFail();
    }
  }

  if (options_.liveNoopPatch) {
    trace.note = trace.note.empty() ? "live instrumentation did not redirect"
                                    : trace.note;
    return hardFail();
  }
  dispatchTraces_.push_back(trace);
  return aegis::hsa_intercept::DispatchDecision::proceed;
}

void Runtime::handleDispatchSubmitted(
    const aegis::hsa_intercept::DispatchEvent &event) {
  if (options_.deferCountingReadback) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pendingCountingDispatches_.find(event.correlationId);
    if (it == pendingCountingDispatches_.end()) {
      return;
    }
    it->second.completionSignal = event.completionSignal;
    it->second.submitted = true;
    return;
  }

  PendingCountingDispatch pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pendingCountingDispatches_.find(event.correlationId);
    if (it == pendingCountingDispatches_.end()) {
      return;
    }
    pending = it->second;
    pendingCountingDispatches_.erase(it);
  }

  auto completion =
      pending.completionObserver->waitForCompletion({event.completionSignal});
  if (pending.ownsCompletionSignal) {
    pending.completionObserver->destroySignal(event.completionSignal);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending.traceIndex >= dispatchTraces_.size()) {
    stats_.hardFailedDispatches++;
    return;
  }
  auto &trace = dispatchTraces_[pending.traceIndex];
  if (!completion) {
    trace.status = "readback-error";
    trace.note = completion.error();
    stats_.hardFailedDispatches++;
    return;
  }

  auto record =
      readCountingRecord(pending.bufferAddress, pending.bufferSize);
  if (!record) {
    trace.status = "readback-error";
    trace.note = record.error();
    stats_.hardFailedDispatches++;
    return;
  }
  trace.profilingRecords.push_back(record.takeValue());
  trace.profilingRecordCount = trace.profilingRecords.back().hitCount;
  trace.note = "counting payload records read after dispatch completion";
  stats_.profilingRecords += trace.profilingRecordCount;
  if (callbacks_.onArtifact && lastRewrite_) {
    callbacks_.onArtifact(
        aegisbit_output::buildReproducerBundle(*lastRewrite_, {trace}));
    stats_.artifactBundles++;
  }
}

void Runtime::flushPendingCountingDispatches() {
  while (true) {
    uint64_t correlationId = 0;
    uint64_t completionSignal = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = pendingCountingDispatches_.begin();
      for (; it != pendingCountingDispatches_.end(); ++it) {
        if (it->second.submitted) {
          break;
        }
      }
      if (it == pendingCountingDispatches_.end()) {
        return;
      }
      correlationId = it->first;
      completionSignal = it->second.completionSignal;
    }
    aegis::hsa_intercept::DispatchEvent event;
    event.correlationId = correlationId;
    event.completionSignal = completionSignal;
    const bool oldDefer = options_.deferCountingReadback;
    options_.deferCountingReadback = false;
    handleDispatchSubmitted(event);
    options_.deferCountingReadback = oldDefer;
  }
}

} // namespace aegisbit_runtime
