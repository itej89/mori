// Copyright © Advanced Micro Devices, Inc. All rights reserved.
// MIT License
#include "mori/core/transport/rdma/proxy/proxy_thread.hpp"

#include <arpa/inet.h>
#include <hip/hip_runtime_api.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace mori {
namespace core {

ProxyThread::~ProxyThread() { Shutdown(); }

void ProxyThread::Init(ProxyRing* ring, std::vector<ProxyQpHandle> qps, int gpuId, int nicId,
                       int numNics, int numQpPerPe) {
  ring_ = ring;
  qps_ = std::move(qps);
  next_slot_ = 0;
  ops_posted_ = 0;
  ops_completed_ = 0;
  gpu_id_ = gpuId;
  nic_id_ = nicId;
  num_nics_ = numNics;
  num_qp_per_pe_ = numQpPerPe;
  pcie_hops_ = 0;
  xgmi_send_ = 0;
  xgmi_recv_ = 0;
}

void ProxyThread::Start() {
  if (running_.load()) return;
  running_.store(true);
  pthread_create(&thread_, nullptr, ThreadFunc, this);
}

void ProxyThread::Shutdown() {
  if (!running_.load()) return;
  if (ring_) ring_->shutdown = 1;
  running_.store(false);
  pthread_join(thread_, nullptr);
  fprintf(stderr, "[HOP-STATS] gpu=%d nic=%d pcie=%lu xgmi_send=%lu xgmi_recv=%lu posted=%lu completed=%lu\n",
          gpu_id_, nic_id_, pcie_hops_, xgmi_send_, xgmi_recv_, ops_posted_, ops_completed_);
}

void* ProxyThread::ThreadFunc(void* arg) {
  auto* self = static_cast<ProxyThread*>(arg);
  self->MainLoop();
  return nullptr;
}

void ProxyThread::DrainCq(ProxyQpHandle& qph) {
  if (!qph.cq) return;
  ibv_wc wc[64];
  int n;
  while ((n = ibv_poll_cq(qph.cq, 64, wc)) > 0) {
    for (int i = 0; i < n; i++) {
      if (wc[i].opcode == IBV_WC_RECV || wc[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
        if (wc[i].status == IBV_WC_SUCCESS && wc[i].byte_len >= 16) {
          uint32_t recv_idx = static_cast<uint32_t>(wc[i].wr_id);
          if (recv_idx < qph.recv_count && qph.recv_buf) {
            struct { uint64_t addr; uint64_t val; } payload;
            memcpy(&payload, reinterpret_cast<char*>(qph.recv_buf) + recv_idx * 64, 16);
            volatile uint64_t* target = reinterpret_cast<volatile uint64_t*>(payload.addr);
            __atomic_fetch_add(target, payload.val, __ATOMIC_SEQ_CST);
            asm volatile("clflush (%0)" :: "r"(target) : "memory");
            asm volatile("sfence" ::: "memory");
            ibv_sge rsge{};
            rsge.addr = reinterpret_cast<uintptr_t>(qph.recv_buf) + recv_idx * 64;
            rsge.length = 64;
            rsge.lkey = qph.recv_lkey;
            ibv_recv_wr rwr{}, *rbad = nullptr;
            rwr.wr_id = recv_idx;
            rwr.sg_list = &rsge;
            rwr.num_sge = 1;
            ibv_post_recv(qph.qp, &rwr, &rbad);
          }
        } else if (wc[i].status != IBV_WC_SUCCESS) {
          fprintf(stderr, "proxy: RECV CQE error status=%d (%s) ibvQP=%u\n",
                  wc[i].status, ibv_wc_status_str(wc[i].status),
                  qph.qp ? qph.qp->qp_num : 0);
        }
        continue;
      }
      uint32_t slot = static_cast<uint32_t>(wc[i].wr_id) & PROXY_RING_MASK;
      if (wc[i].status == IBV_WC_SUCCESS) {
        ring_->cmds[slot].status = PROXY_COMPLETED;
      } else {
        fprintf(stderr, "proxy: CQE error slot=%u status=%d (%s) wr_id=%lu ibvQP=%u\n",
                slot, wc[i].status, ibv_wc_status_str(wc[i].status), wc[i].wr_id,
                qph.qp ? qph.qp->qp_num : 0);
        ring_->cmds[slot].status = PROXY_ERROR;
      }
      ops_completed_++;
    }
  }
}

// Build a single ibv_send_wr from a ProxyCmd. Returns false on invalid op.
bool ProxyThread::BuildWr(volatile ProxyCmd* cmd, ProxyQpHandle& qph,
                          ibv_send_wr& wr, ibv_sge& sge, uint32_t slot_id,
                          InlineBuf& ibuf) {
  sge.addr = cmd->src_addr;
  sge.length = cmd->length;
  sge.lkey = (qph.lkey_override != 0) ? qph.lkey_override : cmd->lkey;

  wr = {};
  wr.wr_id = slot_id;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;

  switch (cmd->op) {
    case PROXY_RDMA_WRITE:
      wr.opcode = IBV_WR_RDMA_WRITE;
      wr.wr.rdma.remote_addr = cmd->dst_addr;
      wr.wr.rdma.rkey = (qph.rkey_override != 0) ? qph.rkey_override : cmd->rkey;
      break;
    case PROXY_RDMA_WRITE_INLINE:
      wr.opcode = IBV_WR_RDMA_WRITE;
      wr.send_flags |= IBV_SEND_INLINE;
      wr.wr.rdma.remote_addr = cmd->dst_addr;
      wr.wr.rdma.rkey = (qph.rkey_override != 0) ? qph.rkey_override : cmd->rkey;
      break;
    case PROXY_SIGNAL_WRITE: {
      ibuf.data[0] = cmd->atomic_arg;
      sge.addr = reinterpret_cast<uintptr_t>(&ibuf.data[0]);
      sge.length = 8;
      sge.lkey = cmd->lkey;
      wr.opcode = IBV_WR_RDMA_WRITE;
      wr.send_flags |= IBV_SEND_INLINE;
      wr.wr.rdma.remote_addr = cmd->dst_addr;
      wr.wr.rdma.rkey = (qph.rkey_override != 0) ? qph.rkey_override : cmd->rkey;
      break;
    }
    case PROXY_ATOMIC_FETCH_ADD:
    case PROXY_ATOMIC_CMP_SWAP: {
      if (qph.use_native_atomics) {
        wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
        wr.wr.atomic.remote_addr = cmd->dst_addr;
        wr.wr.atomic.rkey = (qph.rkey_override != 0) ? qph.rkey_override : cmd->rkey;
        wr.wr.atomic.compare_add = cmd->atomic_arg;
        sge.addr = cmd->src_addr;
        sge.length = 8;
        sge.lkey = cmd->lkey;
      } else {
        ibuf.data[0] = cmd->dst_addr;
        ibuf.data[1] = cmd->atomic_arg;
        sge.addr = reinterpret_cast<uintptr_t>(&ibuf.data[0]);
        sge.length = 16;
        wr.opcode = IBV_WR_SEND_WITH_IMM;
        wr.imm_data = htonl(0xA70C);
        wr.send_flags |= IBV_SEND_INLINE;
      }
      break;
    }
    default:
      return false;
  }
  return true;
}

void ProxyThread::MainLoop() {
  hipSetDevice(gpu_id_);

  static constexpr int kMaxBatch = 64;
  ibv_send_wr wrs[kMaxBatch];
  ibv_sge sges[kMaxBatch];
  InlineBuf ibufs[kMaxBatch];
  uint32_t wr_qp[kMaxBatch];
  int batch_count = 0;

  while (!ring_->shutdown) {
    batch_count = 0;

    uint32_t head = ring_->gpu_head;
    while (next_slot_ < head && batch_count < kMaxBatch) {
      uint32_t slot = next_slot_ & PROXY_RING_MASK;
      volatile ProxyCmd* cmd = &ring_->cmds[slot];

      if (cmd->status != PROXY_PENDING) break;

      uint32_t qi = cmd->qp_idx;
      if (qi >= qps_.size() || qps_[qi].qp == nullptr) {
        cmd->status = PROXY_ERROR;
        next_slot_++;
        continue;
      }

      if (!BuildWr(cmd, qps_[qi], wrs[batch_count], sges[batch_count], next_slot_, ibufs[batch_count])) {
        cmd->status = PROXY_ERROR;
        next_slot_++;
        continue;
      }

      wr_qp[batch_count] = qi;
      wrs[batch_count].next = nullptr;
      next_slot_++;
      batch_count++;
    }

    // Post the batch: group WRs by QP, chain each group, post with one ibv_post_send call
    if (batch_count > 0) {
      // Build per-QP chains: chain_head[qi] points to first WR for that QP
      int chain_head[kMaxBatch];
      int chain_tail[kMaxBatch];
      int num_chains = 0;
      uint32_t seen_qps[kMaxBatch];

      for (int k = 0; k < batch_count; k++) {
        uint32_t qi = wr_qp[k];
        wrs[k].next = nullptr;
        int found = -1;
        for (int c = 0; c < num_chains; c++) {
          if (seen_qps[c] == qi) { found = c; break; }
        }
        if (found >= 0) {
          wrs[chain_tail[found]].next = &wrs[k];
          chain_tail[found] = k;
        } else {
          seen_qps[num_chains] = qi;
          chain_head[num_chains] = k;
          chain_tail[num_chains] = k;
          num_chains++;
        }
      }

      // Post each chain
      for (int c = 0; c < num_chains; c++) {
        uint32_t qi = seen_qps[c];
        ProxyQpHandle& qph = qps_[qi];

        ibv_send_wr* to_post = &wrs[chain_head[c]];
        while (to_post) {
          ibv_send_wr* bad = nullptr;
          int ret = ibv_post_send(qph.qp, to_post, &bad);

          if (ret == 0) {
            ops_posted_++;
            pcie_hops_++;
            if (nic_id_ != gpu_id_) xgmi_send_++;
            int peer = qi / num_qp_per_pe_;
            int peerLocalGpu = peer % num_nics_;
            int recvNic = (gpu_id_ > peerLocalGpu ? gpu_id_ : peerLocalGpu) % num_nics_;
            if (recvNic != peerLocalGpu) xgmi_recv_++;
            if (ops_posted_ <= 3)
              fprintf(stderr, "[HOP-DEBUG] gpu=%d nic=%d qi=%u peer=%d peerLocal=%d recvNic=%d xgmi_recv=%s nqp=%d nnics=%d\n",
                      gpu_id_, nic_id_, qi, peer, peerLocalGpu, recvNic,
                      (recvNic != peerLocalGpu) ? "YES" : "no", num_qp_per_pe_, num_nics_);
            break;
          }

          if (ret == ENOMEM) {
            // SQ full: drain CQEs and retry from the failed WR
            to_post = bad ? bad : to_post;
            for (int attempt = 0; attempt < 10000; attempt++) {
              DrainCq(qph);
              bad = nullptr;
              ret = ibv_post_send(qph.qp, to_post, &bad);
              if (ret == 0) break;
              if (ret == ENOMEM) {
                to_post = bad ? bad : to_post;
              } else {
                break;
              }
            }
            if (ret == 0) {
              ops_posted_++;
              break;
            }
          }

          ibv_send_wr* w = to_post;
          while (w) {
            uint32_t slot = static_cast<uint32_t>(w->wr_id) & PROXY_RING_MASK;
            ring_->cmds[slot].status = PROXY_ERROR;
            w = w->next;
          }
          break;
        }
      }
    }

    // Drain all CQs
    for (auto& qph : qps_) {
      if (qph.qp) DrainCq(qph);
    }
  }
}

}  // namespace core
}  // namespace mori
