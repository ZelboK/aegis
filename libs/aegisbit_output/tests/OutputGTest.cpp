//===-- OutputGTest.cpp - output tests -------------------------*- C++ -*-===//

#include "aegisbit_output/Output.h"

#include <gtest/gtest.h>

TEST(AegisbitOutputTest, RendersRewriteTraceSummaryAndJson) {
  amdgpu_rewrite_core::RewriteTrace trace;
  trace.rewriteId = "rewrite";
  trace.kernel.name = "kernel";
  trace.kernel.arch = "gfx942";
  trace.plan.instrumentation =
      amdgpu_rewrite_core::InstrumentationLevel::dryPayload;
  trace.plan.selectedSites.push_back({});
  trace.patches.push_back({});
  trace.invariants.push_back({"branch-reach",
                              amdgpu_rewrite_core::InvariantStatus::passed,
                              0x1000, "ok"});

  auto summary = aegisbit_output::renderRewriteSummary(trace);
  EXPECT_NE(summary.find("DryPayload"), std::string::npos);
  EXPECT_NE(summary.find("sites=1"), std::string::npos);

  auto json = aegisbit_output::renderRewriteTraceJson(trace);
  EXPECT_NE(json.find("\"rewriteId\":\"rewrite\""), std::string::npos);
  EXPECT_NE(json.find("\"invariants\""), std::string::npos);
}

TEST(AegisbitOutputTest, BuildsReproducerBundle) {
  amdgpu_rewrite_core::RewriteResult rewrite;
  rewrite.patched.bytes = {1, 2, 3};
  rewrite.trace.rewriteId = "rewrite";
  aegisbit_output::DispatchTrace dispatch;
  dispatch.dispatchId = 42;
  dispatch.rewriteId = "rewrite";
  dispatch.status = "observed";

  auto bundle = aegisbit_output::buildReproducerBundle(rewrite, {dispatch});
  ASSERT_EQ(bundle.files.size(), 3u);
  EXPECT_EQ(bundle.files[0].name, "patched_code_object.bin");
  EXPECT_EQ(bundle.files[1].name, "rewrite_trace.json");
  EXPECT_EQ(bundle.files[2].name, "dispatch_0.json");
}
