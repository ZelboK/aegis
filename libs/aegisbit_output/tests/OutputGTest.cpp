//===-- OutputGTest.cpp - output tests -------------------------*- C++ -*-===//

#include "aegisbit_output/Output.h"

#include <gtest/gtest.h>

#include <vector>

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
  dispatch.profilingRecordCount = 7;

  auto bundle = aegisbit_output::buildReproducerBundle(rewrite, {dispatch});
  ASSERT_EQ(bundle.files.size(), 4u);
  EXPECT_EQ(bundle.files[0].name, "patched_code_object.bin");
  EXPECT_EQ(bundle.files[1].name, "rewrite_summary.txt");
  EXPECT_EQ(bundle.files[2].name, "rewrite_trace.json");
  EXPECT_EQ(bundle.files[3].name, "dispatch_0.json");
  std::string dispatchJson(bundle.files[3].bytes.begin(),
                           bundle.files[3].bytes.end());
  EXPECT_NE(dispatchJson.find("\"profilingRecordCount\":7"),
            std::string::npos);
}

TEST(AegisbitOutputTest, BuildsReproducerBundleWithOriginalBytes) {
  amdgpu_rewrite_core::RewriteResult rewrite;
  rewrite.patched.bytes = {4, 5, 6};
  rewrite.trace.rewriteId = "rewrite";
  aegisbit_output::DispatchTrace dispatch;
  dispatch.dispatchId = 42;
  dispatch.rewriteId = "rewrite";
  dispatch.status = "rewritten-artifact";

  auto bundle =
      aegisbit_output::buildReproducerBundle(rewrite, {dispatch}, {1, 2, 3});
  ASSERT_EQ(bundle.files.size(), 5u);
  EXPECT_EQ(bundle.files[0].name, "original_code_object.bin");
  EXPECT_EQ(bundle.files[0].bytes, std::vector<uint8_t>({1, 2, 3}));
  EXPECT_EQ(bundle.files[1].name, "patched_code_object.bin");
}

TEST(AegisbitOutputTest, RendersProfilingRecordsInDispatchAndBundle) {
  amdgpu_rewrite_core::RewriteResult rewrite;
  rewrite.patched.bytes = {1};
  rewrite.trace.rewriteId = "rewrite";
  aegisbit_output::DispatchTrace dispatch;
  dispatch.dispatchId = 7;
  dispatch.rewriteId = "rewrite";
  dispatch.status = "loaded-patched-kernel";
  dispatch.profilingRecordCount = 3;
  dispatch.profilingRecords.push_back(
      {"CountingPayload", "CountingPayloadRecordV1", 0x1000, 9, 3, "read",
       "ok"});

  auto json = aegisbit_output::renderDispatchTraceJson(dispatch);
  EXPECT_NE(json.find("\"profilingRecords\""), std::string::npos);
  EXPECT_NE(json.find("\"siteId\":9"), std::string::npos);
  EXPECT_NE(json.find("\"hitCount\":3"), std::string::npos);

  auto bundle = aegisbit_output::buildReproducerBundle(rewrite, {dispatch});
  ASSERT_EQ(bundle.files.size(), 5u);
  EXPECT_EQ(bundle.files[4].name, "profiling_records.json");
}
