//===-- RuntimeGTest.cpp - runtime composition tests ----------*- C++ -*-===//

#include "aegisbit_runtime/Runtime.h"

#include <gtest/gtest.h>

namespace {

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
