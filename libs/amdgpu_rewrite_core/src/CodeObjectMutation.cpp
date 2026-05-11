//===-- CodeObjectMutation.cpp - LLVM-free code object mutation -----------===//

#include "amdgpu_rewrite_core/CodeObjectMutation.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace amdgpu_rewrite_core {
namespace {

using amdgpu_instr_backend::Result;

uint16_t readLe16(const std::vector<uint8_t> &bytes, size_t offset) {
  if (offset + 2 > bytes.size()) {
    return 0;
  }
  return static_cast<uint16_t>(bytes[offset]) |
         static_cast<uint16_t>(bytes[offset + 1]) << 8;
}

uint64_t readLe64(const std::vector<uint8_t> &bytes, size_t offset) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8 && offset + i < bytes.size(); ++i) {
    value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
  }
  return value;
}

void writeLe64(std::vector<uint8_t> &bytes, size_t offset, uint64_t value) {
  for (size_t i = 0; i < 8 && offset + i < bytes.size(); ++i) {
    bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
  }
}

void updateProgramHeadersForInsert(std::vector<uint8_t> &bytes,
                                   uint64_t insertOffset,
                                   uint64_t payloadSize) {
  const uint64_t programHeaderOffset = readLe64(bytes, 32);
  const uint16_t programHeaderEntrySize = readLe16(bytes, 54);
  const uint16_t programHeaderCount = readLe16(bytes, 56);
  if (programHeaderEntrySize < 56 || programHeaderCount == 0 ||
      programHeaderOffset +
              static_cast<uint64_t>(programHeaderEntrySize) *
                  programHeaderCount >
          bytes.size()) {
    return;
  }

  for (uint16_t i = 0; i < programHeaderCount; ++i) {
    const size_t programHeader =
        static_cast<size_t>(programHeaderOffset) +
        static_cast<size_t>(i) * programHeaderEntrySize;
    const uint64_t segmentOffset = readLe64(bytes, programHeader + 8);
    const uint64_t segmentFileSize = readLe64(bytes, programHeader + 32);
    const uint64_t segmentMemSize = readLe64(bytes, programHeader + 40);
    if (segmentOffset <= insertOffset &&
        insertOffset <= segmentOffset + segmentFileSize) {
      writeLe64(bytes, programHeader + 32, segmentFileSize + payloadSize);
      writeLe64(bytes, programHeader + 40, segmentMemSize + payloadSize);
      continue;
    }
    if (segmentOffset > insertOffset) {
      writeLe64(bytes, programHeader + 8, segmentOffset + payloadSize);
    }
  }
}

bool looksLikeElf64Le(const std::vector<uint8_t> &bytes) {
  return bytes.size() >= 64 && bytes[0] == 0x7f && bytes[1] == 'E' &&
         bytes[2] == 'L' && bytes[3] == 'F' && bytes[4] == 2 &&
         bytes[5] == 1;
}

} // namespace

Result<TextAppendResult> appendToText(const TextAppendRequest &request) {
  TextAppendResult result;
  result.codeObjectBytes = request.codeObjectBytes;
  result.appendedPc = request.textBase + request.textSize;
  result.textSize = request.textSize;

  const uint64_t insertOffset64 = request.textOffset + request.textSize;
  if (insertOffset64 > result.codeObjectBytes.size()) {
    return Result<TextAppendResult>::failure(
        "payload insert offset is outside code object bytes");
  }

  const auto insertOffset = static_cast<size_t>(insertOffset64);
  const uint64_t payloadSize =
      static_cast<uint64_t>(request.bytesToAppend.size());

  if (!looksLikeElf64Le(result.codeObjectBytes)) {
    result.codeObjectBytes.insert(
        result.codeObjectBytes.begin() + static_cast<std::ptrdiff_t>(insertOffset),
        request.bytesToAppend.begin(), request.bytesToAppend.end());
    result.textSize += payloadSize;
    return Result<TextAppendResult>::success(std::move(result));
  }

  const uint64_t oldSectionHeaderOffset = readLe64(result.codeObjectBytes, 40);
  const uint16_t sectionHeaderEntrySize =
      readLe16(result.codeObjectBytes, 58);
  const uint16_t sectionCount = readLe16(result.codeObjectBytes, 60);
  if (sectionHeaderEntrySize < 64 || sectionCount == 0 ||
      oldSectionHeaderOffset +
              static_cast<uint64_t>(sectionHeaderEntrySize) * sectionCount >
          result.codeObjectBytes.size()) {
    return Result<TextAppendResult>::failure(
        "ELF section table is not valid for payload insertion");
  }

  int textSectionIndex = -1;
  std::vector<uint64_t> sectionOffsets(sectionCount, 0);
  for (uint16_t i = 0; i < sectionCount; ++i) {
    const size_t sectionHeader =
        static_cast<size_t>(oldSectionHeaderOffset) +
        static_cast<size_t>(i) * sectionHeaderEntrySize;
    const uint64_t sectionOffset =
        readLe64(result.codeObjectBytes, sectionHeader + 24);
    const uint64_t sectionSize =
        readLe64(result.codeObjectBytes, sectionHeader + 32);
    sectionOffsets[i] = sectionOffset;
    if (sectionOffset == request.textOffset &&
        sectionSize == request.textSize) {
      textSectionIndex = i;
    }
  }
  if (textSectionIndex < 0) {
    return Result<TextAppendResult>::failure(
        "could not find matching ELF .text section for payload insertion");
  }

  result.codeObjectBytes.insert(
      result.codeObjectBytes.begin() + static_cast<std::ptrdiff_t>(insertOffset),
      request.bytesToAppend.begin(), request.bytesToAppend.end());

  updateProgramHeadersForInsert(result.codeObjectBytes, insertOffset64,
                                payloadSize);

  const uint64_t newSectionHeaderOffset =
      oldSectionHeaderOffset >= insertOffset64
          ? oldSectionHeaderOffset + payloadSize
          : oldSectionHeaderOffset;
  if (newSectionHeaderOffset != oldSectionHeaderOffset) {
    writeLe64(result.codeObjectBytes, 40, newSectionHeaderOffset);
  }

  for (uint16_t i = 0; i < sectionCount; ++i) {
    const size_t sectionHeader =
        static_cast<size_t>(newSectionHeaderOffset) +
        static_cast<size_t>(i) * sectionHeaderEntrySize;
    if (i == static_cast<uint16_t>(textSectionIndex)) {
      writeLe64(result.codeObjectBytes, sectionHeader + 32,
                request.textSize + payloadSize);
      continue;
    }
    if (sectionOffsets[i] >= insertOffset64) {
      writeLe64(result.codeObjectBytes, sectionHeader + 24,
                sectionOffsets[i] + payloadSize);
    }
  }

  result.textSize += payloadSize;
  return Result<TextAppendResult>::success(std::move(result));
}

} // namespace amdgpu_rewrite_core
