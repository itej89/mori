// Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
// MIT License
#pragma once

#include "mori/core/transport/rdma/proxy/proxy_types.hpp"

namespace mori {
namespace shmem {

struct ProxyGpuState {
  bool active{false};
  ::mori::core::ProxyRing* rings[::mori::core::PROXY_MAX_NICS]{};
  uint32_t quietHead[::mori::core::PROXY_MAX_NICS]{};
  int numRings{0};
  int numNics{0};
  int localGpuIdx{0};
  int numQpPerPe{4};
};

extern __device__ __attribute__((visibility("default"))) ProxyGpuState globalProxyState;

static __device__ ProxyGpuState* GetGlobalProxyStatePtr() { return &globalProxyState; }

}  // namespace shmem
}  // namespace mori
