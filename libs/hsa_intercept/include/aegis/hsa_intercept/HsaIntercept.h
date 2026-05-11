//===-- aegis/hsa_intercept/HsaIntercept.h ----------------------*- C++ -*-===//

#ifndef AEGIS_HSA_INTERCEPT_HSA_INTERCEPT_H
#define AEGIS_HSA_INTERCEPT_HSA_INTERCEPT_H

#include "aegis/hsa_intercept/CaptureTypes.h"

#include <cstdint>
#include <functional>
#include <string>

namespace aegis::hsa_intercept {

enum class StatusCode {
  ok,
  unavailable,
  invalidArgument,
  apiTableUnavailable,
  queueInterceptUnavailable,
  queueRegistrationFailed,
  realFunctionMissing,
};

class Status {
public:
  Status() = default;
  Status(StatusCode code, std::string message = {});

  [[nodiscard]] static Status success();

  [[nodiscard]] bool ok() const;
  [[nodiscard]] StatusCode code() const;
  [[nodiscard]] const std::string& message() const;

private:
  StatusCode code_ = StatusCode::ok;
  std::string message_;
};

enum class DispatchDecision {
  proceed,
  skip,
};

struct Stats {
  uint64_t totalDispatches = 0;
  uint64_t modifiedDispatches = 0;
  uint64_t skippedDispatches = 0;
  uint64_t errorDispatches = 0;
};

struct Options {
  bool enabled = true;
  bool enableRocprofilerApiTableCapture = true;
};

struct Callbacks {
  std::function<void(const CapturedCodeObject&)> onCodeObject;
  std::function<void(const CapturedKernelSymbol&)> onKernelSymbol;
  std::function<DispatchDecision(DispatchEvent&)> onDispatch;
  std::function<void(const DispatchEvent&)> onDispatchSubmitted;
  std::function<void(const std::string&)> log;
};

[[nodiscard]] Status install(Options options = {}, Callbacks callbacks = {});
void uninstall();

[[nodiscard]] bool isInstalled();
[[nodiscard]] bool isApiTableReady();

void setCallbacks(Callbacks callbacks);
void clearCallbacks();

[[nodiscard]] Stats stats();
void resetStats();

/// Register queue interception for a queue created outside the interposed path.
[[nodiscard]] Status registerQueue(QueueHandle queue);

/// Test/support hook for packet batches supplied by the HSA queue intercept API.
void handlePacketWrite(const void* packets, uint64_t packetCount,
                       uint64_t userData, void* callbackData, void* writerPtr);

} // namespace aegis::hsa_intercept

#endif // AEGIS_HSA_INTERCEPT_HSA_INTERCEPT_H
