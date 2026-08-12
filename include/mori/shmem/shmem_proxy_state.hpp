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

// Device symbol and accessor declared in device-only context
// (shmem_device_api.hpp / ep_common.hip) to avoid host linker issues.

}  // namespace shmem
}  // namespace mori
