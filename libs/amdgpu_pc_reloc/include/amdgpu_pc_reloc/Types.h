//===-- amdgpu_pc_reloc/Types.h - Relocator data types -----------*- C++ -*-===//

#ifndef AMDGPU_PC_RELOC_TYPES_H
#define AMDGPU_PC_RELOC_TYPES_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace amdgpu_pc_reloc {

struct ByteRange {
  uint64_t Start = 0;
  uint64_t End = 0; // exclusive

  [[nodiscard]] bool empty() const { return Start >= End; }
  [[nodiscard]] bool contains(uint64_t Addr) const {
    return Addr >= Start && Addr < End;
  }
  [[nodiscard]] bool overlaps(uint64_t A, uint64_t B) const {
    return Start < B && A < End;
  }
};

enum class DiagnosticSeverity {
  Note,
  Warning,
  Error,
};

enum class DiagnosticCode {
  None,
  DecodeFailed,
  EncodeFailed,
  OutOfRangeBranch,
  UnalignedTarget,
  InvalidOverwriteSize,
  SizeChangingEncode,
  ProtectedRangeOverlap,
  UnsupportedPCRelativeForm,
};

struct Diagnostic {
  DiagnosticSeverity Severity = DiagnosticSeverity::Note;
  DiagnosticCode Code = DiagnosticCode::None;
  uint64_t Address = 0;
  std::string Message;

  [[nodiscard]] static Diagnostic error(DiagnosticCode Code, uint64_t Address,
                                        std::string Message) {
    return {DiagnosticSeverity::Error, Code, Address, std::move(Message)};
  }

  [[nodiscard]] static Diagnostic warning(DiagnosticCode Code,
                                          uint64_t Address,
                                          std::string Message) {
    return {DiagnosticSeverity::Warning, Code, Address, std::move(Message)};
  }
};

struct RelocationRecord {
  uint64_t OriginalAddress = 0;
  uint64_t RelocatedAddress = 0;
  uint64_t TargetBefore = 0;
  uint64_t TargetAfter = 0;
  std::string Kind;
};

inline bool containsAddress(const std::vector<ByteRange> &Ranges,
                            uint64_t Address) {
  for (const auto &R : Ranges)
    if (R.contains(Address))
      return true;
  return false;
}

inline bool overlapsAny(const std::vector<ByteRange> &Ranges, uint64_t Start,
                        uint64_t End) {
  for (const auto &R : Ranges)
    if (R.overlaps(Start, End))
      return true;
  return false;
}

} // namespace amdgpu_pc_reloc

#endif // AMDGPU_PC_RELOC_TYPES_H
