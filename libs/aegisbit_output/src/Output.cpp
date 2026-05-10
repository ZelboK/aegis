//===-- Output.cpp - neutral reports and repro bundles ----------*- C++ -*-===//

#include "aegisbit_output/Output.h"

#include <sstream>

namespace aegisbit_output {
namespace {

std::string escapeJson(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

std::vector<uint8_t> bytesFromString(const std::string &value) {
  return std::vector<uint8_t>(value.begin(), value.end());
}

const char *instrumentationName(amdgpu_rewrite_core::InstrumentationLevel level) {
  switch (level) {
  case amdgpu_rewrite_core::InstrumentationLevel::noopPatch:
    return "NoopPatch";
  case amdgpu_rewrite_core::InstrumentationLevel::dryPayload:
    return "DryPayload";
  case amdgpu_rewrite_core::InstrumentationLevel::countingPayload:
    return "CountingPayload";
  }
  return "Unknown";
}

} // namespace

std::string renderRewriteSummary(
    const amdgpu_rewrite_core::RewriteTrace &trace) {
  std::ostringstream os;
  os << "rewrite " << trace.rewriteId << " kernel=" << trace.kernel.name
     << " instrumentation=" << instrumentationName(trace.plan.instrumentation)
     << " sites=" << trace.plan.selectedSites.size()
     << " patches=" << trace.patches.size()
     << " daisyChains=" << trace.daisyChains.size();
  return os.str();
}

std::string renderRewriteTraceJson(
    const amdgpu_rewrite_core::RewriteTrace &trace) {
  std::ostringstream os;
  os << "{";
  os << "\"rewriteId\":\"" << escapeJson(trace.rewriteId) << "\",";
  os << "\"kernel\":{\"name\":\"" << escapeJson(trace.kernel.name)
     << "\",\"arch\":\"" << escapeJson(trace.kernel.arch)
     << "\",\"kernelObject\":" << trace.kernel.kernelObject
     << ",\"metadataSource\":\"" << escapeJson(trace.kernel.metadataSource)
     << "\"},";
  os << "\"instrumentation\":\""
     << instrumentationName(trace.plan.instrumentation) << "\",";
  os << "\"siteCount\":" << trace.plan.selectedSites.size() << ",";
  os << "\"patchCount\":" << trace.patches.size() << ",";
  os << "\"daisyChainCount\":" << trace.daisyChains.size() << ",";
  os << "\"invariants\":[";
  for (size_t i = 0; i < trace.invariants.size(); ++i) {
    const auto &check = trace.invariants[i];
    if (i != 0) {
      os << ",";
    }
    os << "{\"name\":\"" << escapeJson(check.name) << "\",\"status\":\""
       << (check.status == amdgpu_rewrite_core::InvariantStatus::passed
               ? "passed"
               : "failed")
       << "\",\"address\":" << check.address << ",\"message\":\""
       << escapeJson(check.message) << "\"}";
  }
  os << "]}";
  return os.str();
}

std::string renderDispatchTraceJson(const DispatchTrace &trace) {
  std::ostringstream os;
  os << "{";
  os << "\"dispatchId\":" << trace.dispatchId << ",";
  os << "\"rewriteId\":\"" << escapeJson(trace.rewriteId) << "\",";
  os << "\"originalKernelObject\":" << trace.originalKernelObject << ",";
  os << "\"patchedKernelObject\":" << trace.patchedKernelObject << ",";
  os << "\"originalKernargAddress\":" << trace.originalKernargAddress << ",";
  os << "\"patchedKernargAddress\":" << trace.patchedKernargAddress << ",";
  os << "\"completionSignal\":" << trace.completionSignal << ",";
  os << "\"status\":\"" << escapeJson(trace.status) << "\",";
  os << "\"note\":\"" << escapeJson(trace.note) << "\"";
  os << "}";
  return os.str();
}

ReproducerBundle buildReproducerBundle(
    const amdgpu_rewrite_core::RewriteResult &rewrite,
    const std::vector<DispatchTrace> &dispatches) {
  return buildReproducerBundle(rewrite, dispatches, {});
}

ReproducerBundle buildReproducerBundle(
    const amdgpu_rewrite_core::RewriteResult &rewrite,
    const std::vector<DispatchTrace> &dispatches,
    const std::vector<uint8_t> &originalCodeObjectBytes) {
  ReproducerBundle bundle;
  if (!originalCodeObjectBytes.empty()) {
    bundle.files.push_back({"original_code_object.bin", originalCodeObjectBytes});
  }
  bundle.files.push_back({"patched_code_object.bin", rewrite.patched.bytes});
  bundle.files.push_back(
      {"rewrite_summary.txt", bytesFromString(renderRewriteSummary(rewrite.trace))});
  bundle.files.push_back(
      {"rewrite_trace.json", bytesFromString(renderRewriteTraceJson(rewrite.trace))});
  for (size_t i = 0; i < dispatches.size(); ++i) {
    std::ostringstream name;
    name << "dispatch_" << i << ".json";
    bundle.files.push_back(
        {name.str(), bytesFromString(renderDispatchTraceJson(dispatches[i]))});
  }
  return bundle;
}

} // namespace aegisbit_output
