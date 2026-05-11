//===-- LlvmCodeObjectParser.cpp - LLVM AMDGPU ELF parser -------*- C++ -*-===//

#include "amdgpu_code_object_llvm/LlvmCodeObjectParser.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolicFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace amdgpu_code_object_llvm {
namespace {

using amdgpu_code_object::DescriptorFacts;
using amdgpu_code_object::ParseRequest;
using amdgpu_code_object::ParsedKernelCode;
using amdgpu_code_object::TextSectionFacts;
using amdgpu_instr_backend::Result;
using llvm::object::ELF64LE;
using llvm::object::ELFObjectFile;
using llvm::object::ELFSymbolRef;
using llvm::object::ObjectFile;
using llvm::object::SectionRef;
using llvm::object::SymbolRef;

struct SectionFacts {
  std::string name;
  uint16_t index = 0;
  uint64_t address = 0;
  uint64_t fileOffset = 0;
  uint64_t size = 0;
};

struct SymbolFacts {
  std::string name;
  uint64_t value = 0;
  uint64_t size = 0;
  uint8_t type = 0;
  uint16_t sectionIndex = 0;
};

std::string errorToString(llvm::Error error) {
  return llvm::toString(std::move(error));
}

std::string stripDescriptorSuffix(std::string name) {
  constexpr llvm::StringLiteral suffix(".kd");
  llvm::StringRef ref(name);
  if (ref.ends_with(suffix)) {
    return ref.drop_back(suffix.size()).str();
  }
  return name;
}

bool namesMatchKernel(const std::string &symbolName, const std::string &kernelName) {
  return symbolName == kernelName ||
         stripDescriptorSuffix(symbolName) == stripDescriptorSuffix(kernelName);
}

uint32_t readLe32(const std::vector<uint8_t> &bytes, uint64_t offset) {
  if (offset + 4 > bytes.size()) {
    return 0;
  }
  uint32_t value = 0;
  for (uint64_t i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);
  }
  return value;
}

std::string getGPUArch(uint32_t flags) {
  switch (flags & 0xffu) {
  case 0x02c:
    return "gfx900";
  case 0x02f:
    return "gfx906";
  case 0x030:
    return "gfx908";
  case 0x03f:
    return "gfx90a";
  case 0x04a:
    return "gfx940";
  case 0x04b:
    return "gfx941";
  case 0x04c:
    return "gfx942";
  case 0x04f:
    return "gfx950";
  case 0x033:
    return "gfx1010";
  case 0x036:
    return "gfx1030";
  case 0x041:
    return "gfx1100";
  case 0x048:
    return "gfx1200";
  case 0x049:
    return "gfx1250";
  default:
    return "unknown";
  }
}

uint32_t vgprGranularityForArch(llvm::StringRef arch) {
  if (arch.starts_with("gfx90a") || arch.starts_with("gfx940") ||
      arch.starts_with("gfx942") || arch.starts_with("gfx950") ||
      arch.starts_with("gfx1250")) {
    return 8;
  }
  return 4;
}

uint32_t extractVgprCount(uint32_t pgmRsrc1, uint32_t granularity) {
  return ((pgmRsrc1 & 0x3fu) + 1) * granularity;
}

uint32_t extractSgprCount(uint32_t pgmRsrc1) {
  return (((pgmRsrc1 >> 6) & 0x0fu) + 1) * 8;
}

Result<SectionFacts> getSectionFacts(const ELFObjectFile<ELF64LE> &elf,
                                     const SectionRef &section,
                                     uint16_t index) {
  auto nameOrErr = section.getName();
  if (!nameOrErr) {
    return Result<SectionFacts>::failure(errorToString(nameOrErr.takeError()));
  }

  auto rawSection = elf.getELFFile().getSection(index);
  if (!rawSection) {
    return Result<SectionFacts>::failure(errorToString(rawSection.takeError()));
  }

  SectionFacts facts;
  facts.name = nameOrErr->str();
  facts.index = index;
  facts.address = section.getAddress();
  facts.fileOffset = (*rawSection)->sh_offset;
  facts.size = (*rawSection)->sh_size;
  return Result<SectionFacts>::success(std::move(facts));
}

Result<std::vector<SectionFacts>>
collectSections(const ELFObjectFile<ELF64LE> &elf) {
  std::vector<SectionFacts> sections;
  uint16_t index = 0;
  for (const auto &section : elf.sections()) {
    auto facts = getSectionFacts(elf, section, index);
    if (!facts) {
      return Result<std::vector<SectionFacts>>::failure(facts.error());
    }
    sections.push_back(facts.takeValue());
    ++index;
  }
  return Result<std::vector<SectionFacts>>::success(std::move(sections));
}

const SectionFacts *findSection(const std::vector<SectionFacts> &sections,
                                llvm::StringRef name) {
  auto it = std::find_if(sections.begin(), sections.end(),
                         [name](const SectionFacts &section) {
                           return section.name == name;
                         });
  return it == sections.end() ? nullptr : &*it;
}

uint16_t sectionIndexFor(const ELFObjectFile<ELF64LE> &elf,
                         llvm::object::section_iterator target) {
  uint16_t index = 0;
  for (auto it = elf.section_begin(); it != elf.section_end(); ++it, ++index) {
    if (it == target) {
      return index;
    }
  }
  return 0;
}

Result<std::vector<SymbolFacts>>
collectSymbols(const ELFObjectFile<ELF64LE> &elf) {
  std::vector<SymbolFacts> symbols;
  for (const auto &symbol : elf.symbols()) {
    auto nameOrErr = symbol.getName();
    if (!nameOrErr) {
      return Result<std::vector<SymbolFacts>>::failure(
          errorToString(nameOrErr.takeError()));
    }

    auto valueOrErr = symbol.getValue();
    if (!valueOrErr) {
      return Result<std::vector<SymbolFacts>>::failure(
          errorToString(valueOrErr.takeError()));
    }

    auto sectionOrErr = symbol.getSection();
    if (!sectionOrErr) {
      return Result<std::vector<SymbolFacts>>::failure(
          errorToString(sectionOrErr.takeError()));
    }

    const auto elfSymbol = llvm::cast<ELFSymbolRef>(symbol);
    SymbolFacts facts;
    facts.name = nameOrErr->str();
    facts.value = *valueOrErr;
    facts.size = elfSymbol.getSize();
    facts.type = elfSymbol.getELFType();

    const auto sectionIt = *sectionOrErr;
    if (sectionIt != elf.section_end()) {
      facts.sectionIndex = sectionIndexFor(elf, sectionIt);
    }
    symbols.push_back(std::move(facts));
  }
  return Result<std::vector<SymbolFacts>>::success(std::move(symbols));
}

std::vector<const SymbolFacts *>
textFunctions(const std::vector<SymbolFacts> &symbols, uint16_t textIndex) {
  std::vector<const SymbolFacts *> functions;
  for (const auto &symbol : symbols) {
    if (symbol.sectionIndex == textIndex &&
        symbol.type == llvm::ELF::STT_FUNC) {
      functions.push_back(&symbol);
    }
  }
  std::sort(functions.begin(), functions.end(),
            [](const SymbolFacts *lhs, const SymbolFacts *rhs) {
              return lhs->value < rhs->value;
            });
  return functions;
}

const SymbolFacts *findKernelSymbol(
    const std::vector<const SymbolFacts *> &functions,
    const std::string &kernelName) {
  auto it = std::find_if(functions.begin(), functions.end(),
                         [&kernelName](const SymbolFacts *symbol) {
                           return namesMatchKernel(symbol->name, kernelName);
                         });
  return it == functions.end() ? nullptr : *it;
}

const SymbolFacts *findDescriptorSymbol(const std::vector<SymbolFacts> &symbols,
                                        llvm::StringRef kernelName) {
  const std::string descriptorName = stripDescriptorSuffix(kernelName.str()) + ".kd";
  auto it = std::find_if(symbols.begin(), symbols.end(),
                         [&descriptorName](const SymbolFacts &symbol) {
                           return symbol.name == descriptorName;
                         });
  return it == symbols.end() ? nullptr : &*it;
}

uint64_t inferTextSize(const std::vector<const SymbolFacts *> &functions,
                       const SymbolFacts &kernel, uint64_t textEnd) {
  if (kernel.size != 0) {
    return kernel.size;
  }

  for (const SymbolFacts *function : functions) {
    if (function->value > kernel.value) {
      return function->value - kernel.value;
    }
  }
  return textEnd - kernel.value;
}

class LlvmCodeObjectParser final
    : public amdgpu_code_object::CodeObjectParser {
public:
  [[nodiscard]] Result<ParsedKernelCode>
  parseKernel(const ParseRequest &request) const override {
    if (request.bytes.empty()) {
      return Result<ParsedKernelCode>::failure("code object bytes are empty");
    }
    if (request.kernelName.empty()) {
      return Result<ParsedKernelCode>::failure("kernel name is empty");
    }

    llvm::StringRef bytes(reinterpret_cast<const char *>(request.bytes.data()),
                          request.bytes.size());
    llvm::MemoryBufferRef buffer(bytes, "aegis-code-object");
    auto objectOrErr = ObjectFile::createELFObjectFile(buffer);
    if (!objectOrErr) {
      return Result<ParsedKernelCode>::failure(
          "failed to parse ELF code object: " +
          errorToString(objectOrErr.takeError()));
    }

    const auto *elf =
        llvm::dyn_cast<ELFObjectFile<ELF64LE>>(objectOrErr->get());
    if (!elf) {
      return Result<ParsedKernelCode>::failure(
          "code object is not ELF64 little-endian");
    }

    const auto &header = elf->getELFFile().getHeader();
    if (header.e_machine != llvm::ELF::EM_AMDGPU) {
      return Result<ParsedKernelCode>::failure(
          "ELF object is not an AMDGPU code object");
    }

    auto sections = collectSections(*elf);
    if (!sections) {
      return Result<ParsedKernelCode>::failure(sections.error());
    }
    const auto sectionFacts = sections.takeValue();
    const SectionFacts *textSection = findSection(sectionFacts, ".text");
    if (!textSection) {
      return Result<ParsedKernelCode>::failure(
          "AMDGPU code object does not contain a .text section");
    }

    auto symbols = collectSymbols(*elf);
    if (!symbols) {
      return Result<ParsedKernelCode>::failure(symbols.error());
    }
    const auto symbolFacts = symbols.takeValue();
    const auto functions = textFunctions(symbolFacts, textSection->index);
    const SymbolFacts *kernel =
        findKernelSymbol(functions, request.kernelName);
    if (!kernel) {
      return Result<ParsedKernelCode>::failure(
          "kernel symbol not found: " + request.kernelName);
    }
    if (kernel->value < textSection->address) {
      return Result<ParsedKernelCode>::failure(
          "kernel symbol is before .text section");
    }

    const uint64_t offsetWithinText = kernel->value - textSection->address;
    if (offsetWithinText > textSection->size) {
      return Result<ParsedKernelCode>::failure(
          "kernel symbol is outside .text section");
    }
    const uint64_t textSize =
        inferTextSize(functions, *kernel, textSection->address + textSection->size);
    if (offsetWithinText + textSize > textSection->size) {
      return Result<ParsedKernelCode>::failure(
          "inferred kernel text range extends beyond .text section");
    }

    ParsedKernelCode parsed;
    parsed.name = kernel->name;
    parsed.arch = getGPUArch(header.e_flags);
    parsed.entryPc = kernel->value;
    parsed.textOffset = textSection->fileOffset + offsetWithinText;
    parsed.textSize = textSize;
    parsed.textBase = textSection->address + offsetWithinText;
    parsed.textSection = {textSection->fileOffset, textSection->address,
                          textSection->size, textSection->index};

    const SymbolFacts *descriptor =
        findDescriptorSymbol(symbolFacts, kernel->name);
    if (descriptor) {
      DescriptorFacts facts;
      facts.present = true;
      facts.address = descriptor->value;
      facts.size = descriptor->size;
      const SectionFacts *descriptorSection = nullptr;
      auto descriptorSectionIt =
          std::find_if(sectionFacts.begin(), sectionFacts.end(),
                       [descriptor](const SectionFacts &section) {
                         return section.index == descriptor->sectionIndex;
                       });
      if (descriptorSectionIt != sectionFacts.end()) {
        descriptorSection = &*descriptorSectionIt;
      }
      if (descriptorSection && descriptor->value >= descriptorSection->address) {
        facts.fileOffset =
            descriptorSection->fileOffset +
            (descriptor->value - descriptorSection->address);
      }
      if (facts.fileOffset + 64 <= request.bytes.size()) {
        facts.groupSegmentFixedSize = readLe32(request.bytes, facts.fileOffset);
        facts.privateSegmentFixedSize =
            readLe32(request.bytes, facts.fileOffset + 4);
        facts.kernargSize = readLe32(request.bytes, facts.fileOffset + 8);
        facts.computePgmRsrc3 = readLe32(request.bytes, facts.fileOffset + 44);
        facts.computePgmRsrc1 = readLe32(request.bytes, facts.fileOffset + 48);
        facts.computePgmRsrc2 = readLe32(request.bytes, facts.fileOffset + 52);
        facts.vgprGranularity = vgprGranularityForArch(parsed.arch);
        facts.vgprCount =
            extractVgprCount(facts.computePgmRsrc1, facts.vgprGranularity);
        facts.sgprCount = extractSgprCount(facts.computePgmRsrc1);
      }
      parsed.descriptor = facts;
    }

    return Result<ParsedKernelCode>::success(std::move(parsed));
  }
};

} // namespace

std::unique_ptr<amdgpu_code_object::CodeObjectParser>
createLlvmCodeObjectParser() {
  return std::make_unique<LlvmCodeObjectParser>();
}

} // namespace amdgpu_code_object_llvm
