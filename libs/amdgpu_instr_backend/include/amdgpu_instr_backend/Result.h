//===-- amdgpu_instr_backend/Result.h ---------------------------*- C++ -*-===//

#ifndef AMDGPU_INSTR_BACKEND_RESULT_H
#define AMDGPU_INSTR_BACKEND_RESULT_H

#include <optional>
#include <string>
#include <utility>

namespace amdgpu_instr_backend {

template <typename T> class Result {
public:
  Result() = default;

  [[nodiscard]] static Result success(T Value) {
    Result R;
    R.Value.emplace(std::move(Value));
    return R;
  }

  [[nodiscard]] static Result failure(std::string Message) {
    Result R;
    R.Error = std::move(Message);
    return R;
  }

  [[nodiscard]] bool ok() const { return Value.has_value(); }
  explicit operator bool() const { return ok(); }

  [[nodiscard]] const T &value() const { return *Value; }
  [[nodiscard]] T &value() { return *Value; }
  T takeValue() { return std::move(*Value); }

  [[nodiscard]] const std::string &error() const { return Error; }

private:
  std::optional<T> Value;
  std::string Error;
};

} // namespace amdgpu_instr_backend

#endif // AMDGPU_INSTR_BACKEND_RESULT_H
