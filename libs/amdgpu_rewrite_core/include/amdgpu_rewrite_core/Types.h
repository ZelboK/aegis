//===-- amdgpu_rewrite_core/Types.h -----------------------------*- C++ -*-===//

#ifndef AMDGPU_REWRITE_CORE_TYPES_H
#define AMDGPU_REWRITE_CORE_TYPES_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace amdgpu_rewrite_core {

enum class DiagnosticSeverity {
  note,
  warning,
  error,
};

enum class DiagnosticCode {
  none,
  invalidRequest,
  unsupportedMode,
  patchOutOfRange,
  patchOutsideText,
  encodeFailed,
  invariantFailed,
};

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::note;
  DiagnosticCode code = DiagnosticCode::none;
  uint64_t address = 0;
  std::string message;

  [[nodiscard]] static Diagnostic error(DiagnosticCode code, uint64_t address,
                                        std::string message) {
    return {DiagnosticSeverity::error, code, address, std::move(message)};
  }

  [[nodiscard]] static Diagnostic warning(DiagnosticCode code,
                                          uint64_t address,
                                          std::string message) {
    return {DiagnosticSeverity::warning, code, address, std::move(message)};
  }
};

struct ByteRange {
  uint64_t start = 0;
  uint64_t end = 0; // exclusive

  [[nodiscard]] bool empty() const { return start >= end; }
  [[nodiscard]] bool contains(uint64_t value) const {
    return value >= start && value < end;
  }
};

enum class RewriteLayout {
  singleKernelClone,
};

enum class RegisterMode {
  zeroSgpr,
};

enum class ZeroSgprFlavor {
  withVgprBump,
  withoutVgprBump,
};

enum class InstrumentationLevel {
  noopPatch,
  dryPayload,
  countingPayload,
};

enum class SiteKind {
  unknown,
  globalMemory,
  ldsMemory,
};

enum class InvariantStatus {
  passed,
  failed,
};

} // namespace amdgpu_rewrite_core

#endif // AMDGPU_REWRITE_CORE_TYPES_H
