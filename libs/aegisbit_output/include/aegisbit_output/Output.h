//===-- aegisbit_output/Output.h -------------------------------*- C++ -*-===//

#ifndef AEGISBIT_OUTPUT_OUTPUT_H
#define AEGISBIT_OUTPUT_OUTPUT_H

#include "amdgpu_rewrite_core/Model.h"
#include "amdgpu_rewrite_core/RewriteCore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegisbit_output {

struct ProfilingRecord {
  std::string profileMode;
  std::string abi;
  uint64_t bufferAddress = 0;
  uint64_t siteId = 0;
  uint64_t hitCount = 0;
  std::string status;
  std::string note;
};

struct DispatchTrace {
  uint64_t dispatchId = 0;
  std::string rewriteId;
  uint64_t originalKernelObject = 0;
  uint64_t patchedKernelObject = 0;
  uint64_t originalKernargAddress = 0;
  uint64_t patchedKernargAddress = 0;
  uint64_t completionSignal = 0;
  uint64_t profilingRecordCount = 0;
  std::string status;
  std::string note;
  std::vector<ProfilingRecord> profilingRecords;
};

struct BundleFile {
  std::string name;
  std::vector<uint8_t> bytes;
};

struct ReproducerBundle {
  std::vector<BundleFile> files;
};

[[nodiscard]] std::string
renderRewriteSummary(const amdgpu_rewrite_core::RewriteTrace &trace);

[[nodiscard]] std::string
renderRewriteTraceJson(const amdgpu_rewrite_core::RewriteTrace &trace);

[[nodiscard]] std::string renderDispatchTraceJson(const DispatchTrace &trace);

[[nodiscard]] std::string
renderProfilingRecordsJson(const std::vector<DispatchTrace> &dispatches);

[[nodiscard]] ReproducerBundle
buildReproducerBundle(const amdgpu_rewrite_core::RewriteResult &rewrite,
                      const std::vector<DispatchTrace> &dispatches);

[[nodiscard]] ReproducerBundle
buildReproducerBundle(const amdgpu_rewrite_core::RewriteResult &rewrite,
                      const std::vector<DispatchTrace> &dispatches,
                      const std::vector<uint8_t> &originalCodeObjectBytes);

} // namespace aegisbit_output

#endif // AEGISBIT_OUTPUT_OUTPUT_H
