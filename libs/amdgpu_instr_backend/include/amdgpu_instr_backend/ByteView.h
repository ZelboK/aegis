//===-- amdgpu_instr_backend/ByteView.h -------------------------*- C++ -*-===//

#ifndef AMDGPU_INSTR_BACKEND_BYTE_VIEW_H
#define AMDGPU_INSTR_BACKEND_BYTE_VIEW_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace amdgpu_instr_backend {

class ByteView {
public:
  ByteView() = default;
  ByteView(const uint8_t *Data, size_t Size) : Data(Data), Size(Size) {}
  ByteView(const std::vector<uint8_t> &Bytes)
      : Data(Bytes.data()), Size(Bytes.size()) {}

  template <size_t N>
  ByteView(const uint8_t (&Bytes)[N]) : Data(Bytes), Size(N) {}

  [[nodiscard]] const uint8_t *data() const { return Data; }
  [[nodiscard]] size_t size() const { return Size; }
  [[nodiscard]] bool empty() const { return Size == 0; }

  [[nodiscard]] const uint8_t *begin() const { return Data; }
  [[nodiscard]] const uint8_t *end() const { return Data + Size; }

  [[nodiscard]] ByteView slice(size_t Offset) const {
    if (Offset >= Size) {
      return {};
    }
    return {Data + Offset, Size - Offset};
  }

private:
  const uint8_t *Data = nullptr;
  size_t Size = 0;
};

} // namespace amdgpu_instr_backend

#endif // AMDGPU_INSTR_BACKEND_BYTE_VIEW_H
