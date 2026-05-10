//===-- amdgpu_code_object/CodeObject.h -------------------------*- C++ -*-===//

#ifndef AMDGPU_CODE_OBJECT_CODE_OBJECT_H
#define AMDGPU_CODE_OBJECT_CODE_OBJECT_H

#include "amdgpu_instr_backend/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace amdgpu_code_object {

struct TextSectionFacts {
  uint64_t fileOffset = 0;
  uint64_t address = 0;
  uint64_t size = 0;
  uint16_t sectionIndex = 0;
};

struct DescriptorFacts {
  bool present = false;
  uint64_t fileOffset = 0;
  uint64_t address = 0;
  uint64_t size = 0;
};

struct ParsedKernelCode {
  std::string name;
  std::string arch;
  uint64_t entryPc = 0;
  uint64_t textOffset = 0;
  uint64_t textSize = 0;
  uint64_t textBase = 0;
  TextSectionFacts textSection;
  std::optional<DescriptorFacts> descriptor;
};

struct ParseRequest {
  std::vector<uint8_t> bytes;
  std::string kernelName;
};

class CodeObjectParser {
public:
  virtual ~CodeObjectParser() = default;

  [[nodiscard]] virtual amdgpu_instr_backend::Result<ParsedKernelCode>
  parseKernel(const ParseRequest &request) const = 0;
};

} // namespace amdgpu_code_object

#endif // AMDGPU_CODE_OBJECT_CODE_OBJECT_H
