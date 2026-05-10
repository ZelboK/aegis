//===-- HsaIntercept.cpp - HSA intercept public API ------------*- C++ -*-===//

#include "Internal.h"

#include <utility>

namespace aegis::hsa_intercept {

Status::Status(StatusCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::success() { return {}; }

bool Status::ok() const { return code_ == StatusCode::ok; }

StatusCode Status::code() const { return code_; }

const std::string& Status::message() const { return message_; }

namespace detail {

State& state() {
  static State instance;
  return instance;
}

Status makeStatus(StatusCode code, std::string message) {
  return Status(code, std::move(message));
}

void log(std::string message) {
  Callbacks callbacks;
  {
    std::lock_guard<std::mutex> lock(state().mutex);
    callbacks = state().callbacks;
  }
  if (callbacks.log) {
    callbacks.log(message);
  }
}

void resetCapturedState() {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.trackedQueues.clear();
  s.codeObjects.clear();
}

#if !defined(AEGIS_HAS_HSA_INTERCEPT)
Status captureApiTable(void*) {
  return makeStatus(StatusCode::unavailable,
                    "HSA interception was built without ROCm support");
}

void captureExecutableState(uint64_t) {}

Status registerQueueIntercept(QueueHandle) {
  return makeStatus(StatusCode::unavailable,
                    "HSA interception was built without ROCm support");
}
#endif

} // namespace detail

Status install(Options options, Callbacks callbacks) {
  auto& s = detail::state();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.options = options;
    s.callbacks = std::move(callbacks);
  }

  if (!options.enabled) {
    s.installed.store(false);
    return Status::success();
  }

#if defined(AEGIS_HAS_HSA_INTERCEPT)
  s.installed.store(true);
  detail::log("HSA intercept armed");
  return Status::success();
#else
  s.installed.store(false);
  return detail::makeStatus(StatusCode::unavailable,
                            "HSA interception was built without ROCm support");
#endif
}

void uninstall() {
  auto& s = detail::state();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.callbacks = {};
    s.trackedQueues.clear();
    s.codeObjects.clear();
    s.queueInterceptCreateFn = nullptr;
    s.queueInterceptRegisterFn = nullptr;
    s.originalQueueCreateFn = nullptr;
    s.queueCreateSlot = nullptr;
  }
  s.apiTableReady.store(false);
  s.installed.store(false);
}

bool isInstalled() { return detail::state().installed.load(); }

bool isApiTableReady() { return detail::state().apiTableReady.load(); }

void setCallbacks(Callbacks callbacks) {
  auto& s = detail::state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.callbacks = std::move(callbacks);
}

void clearCallbacks() {
  auto& s = detail::state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.callbacks = {};
}

Stats stats() {
  auto& s = detail::state();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.statistics;
}

void resetStats() {
  auto& s = detail::state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.statistics = {};
}

Status registerQueue(QueueHandle queue) {
  return detail::registerQueueIntercept(queue);
}

void handlePacketWrite(const void* packets, uint64_t packetCount,
                       uint64_t /*userData*/, void* callbackData,
                       void* writerPtr) {
  auto writer = reinterpret_cast<detail::PacketWriter>(writerPtr);
  auto* queue = callbackData;

  if (packetCount == 0) {
    return;
  }
  if (!packets) {
    std::lock_guard<std::mutex> lock(detail::state().mutex);
    detail::state().statistics.errorDispatches++;
    return;
  }

  const auto* aqlPackets =
      static_cast<const detail::AqlKernelDispatchPacket*>(packets);

  for (uint64_t i = 0; i < packetCount; ++i) {
    const auto& packet = aqlPackets[i];
    const uint8_t packetType = static_cast<uint8_t>(packet.header & 0xFFu);

    if (packetType != detail::kKernelDispatchPacketType) {
      if (writer) {
        writer(&packet, 1);
      }
      continue;
    }

    Callbacks callbacks;
    {
      std::lock_guard<std::mutex> lock(detail::state().mutex);
      detail::state().statistics.totalDispatches++;
      callbacks = detail::state().callbacks;
    }

    if (!callbacks.onDispatch) {
      if (writer) {
        writer(&packet, 1);
      }
      continue;
    }

    auto modifiedPacket = packet;
    DispatchEvent event;
    event.queue = queue;
    event.packet.header = packet.header;
    event.packet.kernelObject = packet.kernelObject;
    event.packet.kernargAddress = packet.kernargAddress;
    event.originalKernelObject = packet.kernelObject;
    event.originalKernargAddress = packet.kernargAddress;

    const DispatchDecision decision = callbacks.onDispatch(event);
    if (decision == DispatchDecision::skip) {
      std::lock_guard<std::mutex> lock(detail::state().mutex);
      detail::state().statistics.skippedDispatches++;
      continue;
    }

    modifiedPacket.kernelObject = event.packet.kernelObject;
    modifiedPacket.kernargAddress = event.packet.kernargAddress;

    if (writer) {
      writer(&modifiedPacket, 1);
    }

    if (modifiedPacket.kernelObject != packet.kernelObject ||
        modifiedPacket.kernargAddress != packet.kernargAddress) {
      std::lock_guard<std::mutex> lock(detail::state().mutex);
      detail::state().statistics.modifiedDispatches++;
    }
  }
}

} // namespace aegis::hsa_intercept
