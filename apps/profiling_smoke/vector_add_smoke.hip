#include <hip/hip_runtime.h>

#include <cstdint>
#include <iostream>
#include <vector>

__global__ void aegis_vector_add(const int *a, const int *b, int *out) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < 64) {
    out[i] = a[i] + b[i];
  }
}

bool checkHip(hipError_t status, const char *what) {
  if (status == hipSuccess) {
    return true;
  }
  std::cerr << what << " failed: " << hipGetErrorString(status) << "\n";
  return false;
}

int main() {
  std::vector<int> a(64), b(64), out(64);
  for (int i = 0; i < 64; ++i) {
    a[i] = i;
    b[i] = 2 * i;
  }

  int *da = nullptr;
  int *db = nullptr;
  int *dout = nullptr;
  if (hipMalloc(&da, a.size() * sizeof(int)) != hipSuccess ||
      hipMalloc(&db, b.size() * sizeof(int)) != hipSuccess ||
      hipMalloc(&dout, out.size() * sizeof(int)) != hipSuccess) {
    std::cerr << "hipMalloc failed\n";
    return 1;
  }

  if (!checkHip(hipMemcpy(da, a.data(), a.size() * sizeof(int),
                          hipMemcpyHostToDevice),
                "hipMemcpy(a)") ||
      !checkHip(hipMemcpy(db, b.data(), b.size() * sizeof(int),
                          hipMemcpyHostToDevice),
                "hipMemcpy(b)")) {
    return 1;
  }
  aegis_vector_add<<<1, 64>>>(da, db, dout);
  const hipError_t launchStatus = hipDeviceSynchronize();
  if (!checkHip(hipMemcpy(out.data(), dout, out.size() * sizeof(int),
                          hipMemcpyDeviceToHost),
                "hipMemcpy(out)")) {
    return 1;
  }
  checkHip(hipFree(da), "hipFree(da)");
  checkHip(hipFree(db), "hipFree(db)");
  checkHip(hipFree(dout), "hipFree(dout)");

  if (launchStatus != hipSuccess) {
    std::cerr << "kernel launch failed: " << hipGetErrorString(launchStatus)
              << "\n";
    return 1;
  }
  for (int i = 0; i < 64; ++i) {
    if (out[i] != 3 * i) {
      std::cerr << "bad result at " << i << ": " << out[i] << "\n";
      return 1;
    }
  }
  std::cout << "aegis vector add smoke passed\n";
  return 0;
}
