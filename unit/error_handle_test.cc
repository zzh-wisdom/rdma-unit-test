// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <errno.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "infiniband/verbs.h"
#include "internal/handle_garble.h"
#include "public/introspection.h"
#include "public/rdma_memblock.h"
#include "public/status_matchers.h"
#include "public/verbs_helper_suite.h"
#include "public/verbs_util.h"
#include "unit/loopback_fixture.h"
#include "unit/rdma_verbs_fixture.h"

/**
 * @brief 说明
 * 
 * 该测试与mlx5的有些行为不一致
 * 仅用于测试srdma的错误处理
 * 
 * 例如:
 * 1. 测试样例PostMulSendButRemoteQpErr，对于mlx5
 * 远端Qp错误时，本地发起Wr结果轮询不到CQE
 * 2. ERR时，CQE的opcode的值混乱，没有与相应的WQE对应
 * 
 */

namespace rdma_unit_test {

using ::testing::AnyOf;
using ::testing::IsNull;
using ::testing::NotNull;
using verbs_util::GetQpState;

class PostWrWhenQpErrorTest : public RdmaVerbsFixture {
 protected:
  static constexpr uint32_t kClientMemoryPages = 1;
  static constexpr uint64_t kCompareAdd = 1;
  static constexpr uint64_t kSwap = 1;
  static constexpr int kDoorbellNum = 7;

  struct BasicSetup {
    RdmaMemBlock buffer;
    ibv_context* context;
    PortAttribute port_attr;
    ibv_cq* local_cq;
    ibv_cq* remote_cq;
    ibv_pd* qp_pd;
    ibv_pd* other_pd;
    ibv_qp* local_qp;
    ibv_qp* remote_qp;
  };

  absl::StatusOr<BasicSetup> CreateBasicSetup() {
    BasicSetup setup;
    setup.buffer = ibv_.AllocBuffer(kClientMemoryPages);
    ASSIGN_OR_RETURN(setup.context, ibv_.OpenDevice());
    setup.port_attr = ibv_.GetPortAttribute(setup.context);
    setup.local_cq = ibv_.CreateCq(setup.context);
    if (!setup.local_cq) {
      return absl::InternalError("Failed to create local cq.");
    }
    setup.remote_cq = ibv_.CreateCq(setup.context);
    if (!setup.remote_cq) {
      return absl::InternalError("Failed to create remote cq.");
    }
    setup.qp_pd = ibv_.AllocPd(setup.context);
    if (!setup.qp_pd) {
      return absl::InternalError("Failed to the qp's pd.");
    }
    setup.other_pd = ibv_.AllocPd(setup.context);
    if (!setup.other_pd) {
      return absl::InternalError("Failed to another pd.");
    }
    setup.local_qp = ibv_.CreateQp(setup.qp_pd, setup.local_cq);
    if (!setup.local_qp) {
      return absl::InternalError("Failed to create local qp.");
    }
    setup.remote_qp = ibv_.CreateQp(setup.qp_pd, setup.remote_cq);
    if (!setup.remote_qp) {
      return absl::InternalError("Failed to create remote qp.");
    }
    RETURN_IF_ERROR(ibv_.SetUpLoopbackRcQps(setup.local_qp, setup.remote_qp,
                                            setup.port_attr));
    return setup;
  }

  static const char* QpStateToStr(ibv_qp_state state) {
    switch (state)
    {
    case IBV_QPS_RESET:
      return "IBV_QPS_RESET";
    case IBV_QPS_INIT:
      return "IBV_QPS_INIT";
    case IBV_QPS_RTR:
      return "IBV_QPS_RTR";
    case IBV_QPS_RTS:
      return "IBV_QPS_RTS";
    case IBV_QPS_SQD:
      return "IBV_QPS_SQD";
    case IBV_QPS_SQE:
      return "IBV_QPS_SQE";
    case IBV_QPS_ERR:
      return "IBV_QPS_ERR";
    case IBV_QPS_UNKNOWN:
      return "IBV_QPS_UNKNOWN";
    default:
      return "Err";
    }
  }

  void QueryQpState(const char* tips, BasicSetup& setup) {
    if(tips) LOG(INFO) << tips;
    LOG(INFO) << "local_qp state: " << QpStateToStr(GetQpState(setup.local_qp));
    LOG(INFO) << "remote_qp state: " << QpStateToStr(GetQpState(setup.remote_qp));
    std::cout << "local_qp state: " << QpStateToStr(GetQpState(setup.local_qp)) << std::endl;
    std::cout << "remote_qp state: " << QpStateToStr(GetQpState(setup.remote_qp)) << std::endl;
  }
};

TEST_F(PostWrWhenQpErrorTest, PostMulSendRecvButFirstSendErr) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr_err = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(local_mr_err, NotNull());
  ibv_mr* local_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr_right, NotNull());
  ibv_mr* remote_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr_right, NotNull());
  uint64_t wr_id_num = 0;

  ibv_sge local_sg_err = verbs_util::CreateSge(setup.buffer.span(), local_mr_err);
  ibv_sge local_sg_right = verbs_util::CreateSge(setup.buffer.span(), local_mr_right);

  ibv_recv_wr recv_wrs[kDoorbellNum];
  for(int i = 0; i < kDoorbellNum; ++i) {
    recv_wrs[i] = verbs_util::CreateRecvWr(
        wr_id_num++, &local_sg_right, /*num_sge=*/1);
    if(i > 0) recv_wrs[i-1].next = &recv_wrs[i];
  }
  recv_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostRecv(setup.local_qp, recv_wrs[0]);

  ibv_send_wr write_wrs[kDoorbellNum];
  // 构建第一个导致失败的WR
  ibv_send_wr& write_first = write_wrs[0];
  write_first = verbs_util::CreateWriteWr(
      wr_id_num++, &local_sg_err, /*num_sge=*/1, setup.buffer.data(), remote_mr_right->rkey);
  for(int i = 1; i < kDoorbellNum; ++i) {
    write_wrs[i] = verbs_util::CreateWriteWr(
        wr_id_num++, &local_sg_right, /*num_sge=*/1, setup.buffer.data(), remote_mr_right->rkey);
    write_wrs[i-1].next = &write_wrs[i];
  }
  write_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostSend(setup.local_qp, write_wrs[0]);

  // sq wr
  wr_id_num = kDoorbellNum;
  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR); // IBV_WC_LOC_PROT_ERR IBV_WC_SUCCESS
  EXPECT_EQ(completion.wr_id, wr_id_num++);
  // EXPECT_EQ(completion.opcode, IBV_WC_RDMA_WRITE);
  for(int i = 1; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    EXPECT_EQ(completion.wr_id, wr_id_num++);
    // EXPECT_EQ(completion.opcode, IBV_WC_RDMA_WRITE);
  }
  // rq wr
  wr_id_num = 0;
  for(int i = 0; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    EXPECT_EQ(completion.wr_id, wr_id_num++);
    EXPECT_TRUE(completion.opcode & IBV_WC_RECV);
  }
  ASSERT_FALSE(verbs_util::WaitForCompletion(setup.local_cq, absl::Seconds(1)).status().ok());
  QueryQpState(nullptr, setup);
}

TEST_F(PostWrWhenQpErrorTest, PostMulRecvButFirstRecvErr) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr_right, NotNull());
  ibv_mr* remote_mr_err = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(remote_mr_err, NotNull());
  ibv_mr* remote_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr_right, NotNull());

  ibv_sge local_sg_right = verbs_util::CreateSge(setup.buffer.span(), local_mr_right);
  ibv_sge remote_sg_err = verbs_util::CreateSge(setup.buffer.span(), remote_mr_err);
  ibv_sge remote_sg_right = verbs_util::CreateSge(setup.buffer.span(), remote_mr_right);

  uint64_t wr_id_num = 0;
  ibv_recv_wr recv_wrs[kDoorbellNum];
  // 构建第一个导致失败的WR
  ibv_recv_wr& recv_first = recv_wrs[0];
  recv_first = verbs_util::CreateRecvWr(
      wr_id_num++, &remote_sg_err, /*num_sge=*/1);
  for(int i = 1; i < kDoorbellNum; ++i) {
    recv_wrs[i] = verbs_util::CreateRecvWr(
        wr_id_num++, &remote_sg_right, /*num_sge=*/1);
    recv_wrs[i-1].next = &recv_wrs[i];
  }
  recv_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostRecv(setup.remote_qp, recv_wrs[0]);

  ibv_send_wr send =
      verbs_util::CreateSendWr(wr_id_num++, &local_sg_right, /*num_sge=1*/ 1);
  verbs_util::PostSend(setup.local_qp, send);

  wr_id_num = 0;
  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.remote_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR); // IBV_WC_WR_FLUSH_ERR
  EXPECT_EQ(completion.wr_id, wr_id_num++);
  // EXPECT_TRUE(completion.opcode & IBV_WC_RECV);
  for(int i = 1; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.remote_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    EXPECT_EQ(completion.wr_id, wr_id_num++);
    // EXPECT_TRUE(completion.opcode & IBV_WC_RECV);
  }
  ASSERT_FALSE(verbs_util::WaitForCompletion(setup.remote_cq, absl::Seconds(1)).status().ok());
  QueryQpState(nullptr, setup);
}

TEST_F(PostWrWhenQpErrorTest, PostMulSendRecvButRemoteRecvErr) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr_right, NotNull());
  ibv_mr* remote_mr_err = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(remote_mr_err, NotNull());
  ibv_mr* remote_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr_right, NotNull());

  ibv_sge local_sg_right = verbs_util::CreateSge(setup.buffer.span(), local_mr_right);
  ibv_sge remote_sg_err = verbs_util::CreateSge(setup.buffer.span(), remote_mr_err);
  ibv_sge remote_sg_right = verbs_util::CreateSge(setup.buffer.span(), remote_mr_right);

  uint64_t wr_id_num = 0;
  // 远端
  ibv_recv_wr remote_recv_wrs[kDoorbellNum];
  // 构建第一个导致失败的WR
  ibv_recv_wr& recv_first = remote_recv_wrs[0];
  recv_first = verbs_util::CreateRecvWr(
      wr_id_num++, &remote_sg_err, /*num_sge=*/1);
  for(int i = 1; i < kDoorbellNum; ++i) {
    remote_recv_wrs[i] = verbs_util::CreateRecvWr(
        wr_id_num++, &remote_sg_right, /*num_sge=*/1);
    remote_recv_wrs[i-1].next = &remote_recv_wrs[i];
  }
  remote_recv_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostRecv(setup.remote_qp, remote_recv_wrs[0]);

  // 本地
  ibv_recv_wr local_recv_wrs[kDoorbellNum];
  for(int i = 0; i < kDoorbellNum; ++i) {
    local_recv_wrs[i] = verbs_util::CreateRecvWr(
        wr_id_num++, &local_sg_right, /*num_sge=*/1);
    if(i > 0) local_recv_wrs[i-1].next = &local_recv_wrs[i];
  }
  local_recv_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostRecv(setup.local_qp, local_recv_wrs[0]);

  ibv_send_wr local_send_wrs[kDoorbellNum];
  for(int i = 0; i < kDoorbellNum; ++i) {
    local_send_wrs[i] = verbs_util::CreateSendWr(
        wr_id_num++, &local_sg_right, /*num_sge=*/1);
    if(i > 0) local_send_wrs[i-1].next = &local_send_wrs[i];
  }
  local_send_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostSend(setup.local_qp, local_send_wrs[0]);

  wr_id_num = 0;
  // 远端轮询cq
  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.remote_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR); // IBV_WC_LOC_PROT_ERR IBV_WC_SUCCESS
  EXPECT_EQ(completion.wr_id, wr_id_num++);
  // EXPECT_TRUE(completion.opcode & IBV_WC_RECV);
  for(int i = 1; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.remote_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    EXPECT_EQ(completion.wr_id, wr_id_num++);
    // EXPECT_TRUE(completion.opcode & IBV_WC_RECV);
  }
  ASSERT_FALSE(verbs_util::WaitForCompletion(setup.remote_cq, absl::Seconds(1)).status().ok());

  // 本地轮询cq
  // send wr
  wr_id_num = 2*kDoorbellNum;
  ASSERT_OK_AND_ASSIGN(completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_REM_OP_ERR); // IBV_WC_WR_FLUSH_ERR
  EXPECT_EQ(completion.wr_id, wr_id_num++);
  // EXPECT_EQ(completion.opcode, IBV_WC_SEND);
  for(int i = 1; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    EXPECT_EQ(completion.wr_id, wr_id_num++);
    // EXPECT_EQ(completion.opcode, IBV_WC_SEND);
  }
  // recv wr
  wr_id_num = kDoorbellNum;
  for(int i = 0; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    EXPECT_EQ(completion.wr_id, wr_id_num++);
    EXPECT_TRUE(completion.opcode & IBV_WC_RECV);
  }
  ASSERT_FALSE(verbs_util::WaitForCompletion(setup.local_cq, absl::Seconds(1)).status().ok());
  QueryQpState(nullptr, setup);
}

TEST_F(PostWrWhenQpErrorTest, RemotePostMulRecvButLocalWriteErr) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr_right, NotNull());
  ibv_mr* remote_mr_err = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(remote_mr_err, NotNull());
  ibv_mr* remote_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr_right, NotNull());

  ibv_sge local_sg_right = verbs_util::CreateSge(setup.buffer.span(), local_mr_right);
  ibv_sge remote_sg_right = verbs_util::CreateSge(setup.buffer.span(), remote_mr_right);

  uint64_t wr_id_num = 0;
  // 远端
  ibv_recv_wr remote_recv_wrs[kDoorbellNum];
  for(int i = 0; i < kDoorbellNum; ++i) {
    remote_recv_wrs[i] = verbs_util::CreateRecvWr(
        wr_id_num++, &remote_sg_right, /*num_sge=*/1);
    if(i > 0) remote_recv_wrs[i-1].next = &remote_recv_wrs[i];
  }
  remote_recv_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostRecv(setup.remote_qp, remote_recv_wrs[0]);

  // 本地
  ibv_send_wr local_write = verbs_util::CreateWriteWr(
    wr_id_num++, &local_sg_right, /*num_sge=*/1, setup.buffer.data(), remote_mr_err->rkey);
  verbs_util::PostSend(setup.local_qp, local_write);

  wr_id_num = 10;
  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_REM_ACCESS_ERR);
  EXPECT_EQ(completion.wr_id, wr_id_num++);

  wr_id_num = 0;
  // 远端轮询cq
  for(int i = 0; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.remote_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    EXPECT_EQ(completion.wr_id, wr_id_num++);
    // EXPECT_TRUE(completion.opcode & IBV_WC_RECV);
  }
  ASSERT_FALSE(verbs_util::WaitForCompletion(setup.remote_cq, absl::Seconds(1)).status().ok());
  QueryQpState(nullptr, setup);
}

TEST_F(PostWrWhenQpErrorTest, LocalPostMulRecvButLocalWriteErr) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr_err = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(local_mr_err, NotNull());
  ibv_mr* local_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr_right, NotNull());
  ibv_mr* remote_mr_err = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(remote_mr_err, NotNull());
  ibv_mr* remote_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr_right, NotNull());

  ibv_sge local_sg_err = verbs_util::CreateSge(setup.buffer.span(), local_mr_err);
  ibv_sge remote_sg_right = verbs_util::CreateSge(setup.buffer.span(), remote_mr_right);

  uint64_t wr_id_num = 0;
  ibv_recv_wr local_recv_wrs[kDoorbellNum];
  for(int i = 0; i < kDoorbellNum; ++i) {
    local_recv_wrs[i] = verbs_util::CreateRecvWr(
        wr_id_num++, &remote_sg_right, /*num_sge=*/1);
    if(i > 0) local_recv_wrs[i-1].next = &local_recv_wrs[i];
  }
  local_recv_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostRecv(setup.local_qp, local_recv_wrs[0]);

  ibv_send_wr local_write = verbs_util::CreateWriteWr(
    wr_id_num++, &local_sg_err, /*num_sge=*/1, setup.buffer.data(), remote_mr_right->rkey);
  verbs_util::PostSend(setup.local_qp, local_write);

  wr_id_num = kDoorbellNum;
  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR);
  EXPECT_EQ(completion.wr_id, wr_id_num++);

  wr_id_num = 0;
  // 远端轮询cq
  for(int i = 0; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    EXPECT_EQ(completion.wr_id, wr_id_num++);
    // EXPECT_TRUE(completion.opcode & IBV_WC_RECV);
  }
  ASSERT_FALSE(verbs_util::WaitForCompletion(setup.remote_cq, absl::Seconds(1)).status().ok());
  QueryQpState(nullptr, setup);
}

TEST_F(PostWrWhenQpErrorTest, PostMulSendButLocalQpErr) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr_err = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(local_mr_err, NotNull());
  ibv_mr* local_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr_right, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge local_sg_err = verbs_util::CreateSge(setup.buffer.span(), local_mr_err);
  ibv_sge local_sg_right = verbs_util::CreateSge(setup.buffer.span(), local_mr_right);

  ibv_send_wr write_err = verbs_util::CreateWriteWr(
      /*wr_id=*/1, &local_sg_err, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey);
  verbs_util::PostSend(setup.local_qp, write_err);
  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR); // IBV_WC_WR_FLUSH_ERR

  uint64_t wr_id_num = 0;
  ibv_send_wr write_wrs[kDoorbellNum];
  for(int i = 0; i < kDoorbellNum; ++i) {
    write_wrs[i] = verbs_util::CreateWriteWr(
        wr_id_num++, &local_sg_right, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey);
    if(i > 0) write_wrs[i-1].next = &write_wrs[i];
  }
  write_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostSend(setup.local_qp, write_wrs[0]);

  wr_id_num = 0;
  for(int i = 0; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    EXPECT_EQ(completion.wr_id, wr_id_num++);
    // EXPECT_EQ(completion.opcode, IBV_WC_RDMA_WRITE);
  }
  ASSERT_FALSE(verbs_util::WaitForCompletion(setup.local_cq, absl::Seconds(1)).status().ok());
  QueryQpState(nullptr, setup);
}

TEST_F(PostWrWhenQpErrorTest, PostMulRecvButLocalQpErr) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr_err = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(local_mr_err, NotNull());
  ibv_mr* local_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr_right, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge local_sg_err = verbs_util::CreateSge(setup.buffer.span(), local_mr_err);
  ibv_sge local_sg_right = verbs_util::CreateSge(setup.buffer.span(), local_mr_right);

  ibv_send_wr write_err = verbs_util::CreateWriteWr(
      /*wr_id=*/1, &local_sg_err, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey);
  verbs_util::PostSend(setup.local_qp, write_err);
  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR); // IBV_WC_WR_FLUSH_ERR

  uint64_t wr_id_num = 0;
  ibv_recv_wr recv_wrs[kDoorbellNum];
  for(int i = 0; i < kDoorbellNum; ++i) {
    recv_wrs[i] = verbs_util::CreateRecvWr(
        wr_id_num++, &local_sg_right, /*num_sge=*/1);
    if(i > 0) recv_wrs[i-1].next = &recv_wrs[i];
  }
  recv_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostRecv(setup.local_qp, recv_wrs[0]);

  wr_id_num = 0;
  for(int i = 0; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    EXPECT_EQ(completion.wr_id, wr_id_num++);
    // EXPECT_TRUE(completion.opcode & IBV_WC_RECV);
  }
  ASSERT_FALSE(verbs_util::WaitForCompletion(setup.local_cq, absl::Seconds(1)).status().ok());
  QueryQpState(nullptr, setup);
}

TEST_F(PostWrWhenQpErrorTest, PostTwoWrButSecondOneErr) {
  auto local_large_buffer = ibv_.AllocBuffer(1024);
  auto remote_large_buffer = ibv_.AllocBuffer(1024);
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr_err = ibv_.RegMr(setup.other_pd, local_large_buffer);
  ASSERT_THAT(local_mr_err, NotNull());
  ibv_mr* local_mr_right = ibv_.RegMr(setup.qp_pd, local_large_buffer);
  ASSERT_THAT(local_mr_right, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.qp_pd, remote_large_buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge local_sg_err = verbs_util::CreateSge(local_large_buffer.span(), local_mr_err);
  ibv_sge local_sg_right = verbs_util::CreateSge(local_large_buffer.span(), local_mr_right);
  
  ibv_send_wr local_wrs[2];
  local_wrs[0] = verbs_util::CreateReadWr(
      /*wr_id=*/1, &local_sg_right, /*num_sge=*/1, remote_large_buffer.data(), remote_mr->rkey);
  local_wrs[1] = verbs_util::CreateWriteWr(
      /*wr_id=*/2, &local_sg_err, /*num_sge=*/1, remote_large_buffer.data(), remote_mr->rkey);
  local_wrs[0].next = &local_wrs[1];
  local_wrs[1].next = nullptr;
  verbs_util::PostSend(setup.local_qp, local_wrs[0]);
  
  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_SUCCESS); // IBV_WC_WR_FLUSH_ERR
  EXPECT_EQ(completion.wr_id, 1);

  ASSERT_OK_AND_ASSIGN(completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR); // IBV_WC_WR_FLUSH_ERR
  EXPECT_EQ(completion.wr_id, 2);

  ASSERT_FALSE(verbs_util::WaitForCompletion(setup.local_cq, absl::Seconds(1)).status().ok());
  QueryQpState(nullptr, setup);
}

TEST_F(PostWrWhenQpErrorTest, PostMulSendButRemoteQpErr) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr_right, NotNull());
  ibv_mr* remote_mr_err = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(remote_mr_err, NotNull());
  ibv_mr* remote_mr_right = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr_right, NotNull());

  ibv_sge remote_sg_err = verbs_util::CreateSge(setup.buffer.span(), remote_mr_err);
  ibv_sge local_sg_right = verbs_util::CreateSge(setup.buffer.span(), local_mr_right);

  ibv_send_wr r_write_err = verbs_util::CreateWriteWr(
      /*wr_id=*/1, &remote_sg_err, /*num_sge=*/1, setup.buffer.data(), local_mr_right->rkey);
  verbs_util::PostSend(setup.remote_qp, r_write_err);
  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.remote_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR); // IBV_WC_WR_FLUSH_ERR

  QueryQpState("Start State:", setup);

  ibv_send_wr write_wrs[kDoorbellNum];
  for(int i = 0; i < kDoorbellNum; ++i) {
    write_wrs[i] = verbs_util::CreateWriteWr(
        /*wr_id=*/i, &local_sg_right, /*num_sge=*/1, setup.buffer.data(), remote_mr_right->rkey);
    if(i > 0) write_wrs[i-1].next = &write_wrs[i];
  }

  write_wrs[kDoorbellNum - 1].next = nullptr;
  verbs_util::PostSend(setup.local_qp, write_wrs[0]);

  ASSERT_OK_AND_ASSIGN(completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_REM_OP_ERR); // IBV_WC_REM_OP_ERR
  for(int i = 1; i < kDoorbellNum; ++i) {
    ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
    EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
  }
  QueryQpState("End State:", setup);
}

class QpErrAndContinueTest : public RdmaVerbsFixture {
 protected:
  static constexpr uint32_t kClientMemoryPages = 1;
  static constexpr uint64_t kCompareAdd = 1;
  static constexpr uint64_t kSwap = 1;

  struct BasicSetup {
    RdmaMemBlock buffer;
    ibv_context* context;
    PortAttribute port_attr;
    ibv_cq* local_cq;
    ibv_cq* remote_cq;
    ibv_pd* qp_pd;
    ibv_pd* other_pd;
    ibv_qp* local_qp;
    ibv_qp* remote_qp;
  };

  absl::StatusOr<BasicSetup> CreateBasicSetup() {
    BasicSetup setup;
    setup.buffer = ibv_.AllocBuffer(kClientMemoryPages);
    ASSIGN_OR_RETURN(setup.context, ibv_.OpenDevice());
    setup.port_attr = ibv_.GetPortAttribute(setup.context);
    setup.local_cq = ibv_.CreateCq(setup.context);
    if (!setup.local_cq) {
      return absl::InternalError("Failed to create local cq.");
    }
    setup.remote_cq = ibv_.CreateCq(setup.context);
    if (!setup.remote_cq) {
      return absl::InternalError("Failed to create remote cq.");
    }
    setup.qp_pd = ibv_.AllocPd(setup.context);
    if (!setup.qp_pd) {
      return absl::InternalError("Failed to the qp's pd.");
    }
    setup.other_pd = ibv_.AllocPd(setup.context);
    if (!setup.other_pd) {
      return absl::InternalError("Failed to another pd.");
    }
    setup.local_qp = ibv_.CreateQp(setup.qp_pd, setup.local_cq);
    if (!setup.local_qp) {
      return absl::InternalError("Failed to create local qp.");
    }
    setup.remote_qp = ibv_.CreateQp(setup.qp_pd, setup.remote_cq);
    if (!setup.remote_qp) {
      return absl::InternalError("Failed to create remote qp.");
    }
    RETURN_IF_ERROR(ibv_.SetUpLoopbackRcQps(setup.local_qp, setup.remote_qp,
                                            setup.port_attr));
    return setup;
  }
  void SendRecvPingPongNormalAfterErr(BasicSetup& setup) {
    // ibv_mr* local_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
    // ASSERT_THAT(local_mr, NotNull());
    // ibv_mr* remote_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
    // ASSERT_THAT(remote_mr, NotNull());

    // ibv_sge remote_rsge = verbs_util::CreateSge(setup.buffer.span(), remote_mr);
    // ibv_sge local_rsge = verbs_util::CreateSge(setup.buffer.span(), local_mr);
    // ibv_recv_wr remote_recv =
    //     verbs_util::CreateRecvWr(/*wr_id=*/0, &remote_rsge, /*num_sge=*/1);
    // verbs_util::PostRecv(setup.remote_qp, remote_recv);
    // ibv_recv_wr local_recv =
    //     verbs_util::CreateRecvWr(/*wr_id=*/0, &local_rsge, /*num_sge=*/1);
    // verbs_util::PostRecv(setup.local_qp, local_recv);

    // remote to local
    // ibv_send_wr remote_write = verbs_util::CreateWriteWr(
    //   /*wr_id=*/1, &remote_rsge, /*num_sge=*/1, setup.buffer.data(), local_mr->rkey);
    // verbs_util::PostSend(setup.remote_qp, remote_write);
    // ASSERT_OK_AND_ASSIGN(ibv_wc completion,
    //                    verbs_util::WaitForCompletion(setup.remote_cq));
    // EXPECT_EQ(completion.status, IBV_WC_SUCCESS);

    // ibv_sge remote_ssge = verbs_util::CreateSge(setup.buffer.span(), remote_mr);
    // ibv_send_wr remote_send =
    //     verbs_util::CreateSendWr(/*wr_id=*/1, &remote_ssge, /*num_sge=1*/ 1);
    // verbs_util::PostSend(setup.remote_qp, remote_send);
    // ASSERT_OK_AND_ASSIGN(completion,
    //                    verbs_util::WaitForCompletion(setup.local_cq));
    // EXPECT_EQ(completion.status, IBV_WC_SUCCESS);
    // ASSERT_OK_AND_ASSIGN(completion,
    //                    verbs_util::WaitForCompletion(setup.remote_cq));
    // EXPECT_EQ(completion.status, IBV_WC_SUCCESS); // IBV_WC_WR_FLUSH_ERR

    // local to remote
    // ibv_sge local_ssge = verbs_util::CreateSge(setup.buffer.span(), local_mr);
    // ibv_send_wr local_send =
    //     verbs_util::CreateSendWr(/*wr_id=*/1, &local_ssge, /*num_sge=1*/ 1);
    // verbs_util::PostSend(setup.local_qp, local_send);
    // ASSERT_OK_AND_ASSIGN(completion,
    //                    verbs_util::WaitForCompletion(setup.local_cq));
    // EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR);
    // ASSERT_OK_AND_ASSIGN(completion,
    //                    verbs_util::WaitForCompletion(setup.remote_cq));
    // EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR);
    // for(int i = 0; i < 10; ++i) {
    //   ibv_send_wr write = verbs_util::CreateWriteWr(
    //       /*wr_id=*/i, &local_rsge, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey);
    //   verbs_util::PostSend(setup.local_qp, write);
    //   ASSERT_OK_AND_ASSIGN(ibv_wc completion,
    //                        verbs_util::WaitForCompletion(setup.local_cq));
    //   EXPECT_EQ(completion.status, IBV_WC_WR_FLUSH_ERR); // IBV_WC_WR_FLUSH_ERR
    // }

    // ibv_send_wr write_wrs[kDoorbellNum];
    // for(int i = 0; i < kDoorbellNum; ++i) {
    //   write_wrs[i] = verbs_util::CreateWriteWr(
    //       /*wr_id=*/i, &local_rsge, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey);
    //   if(i > 0) {
    //     write_wrs[i-1].next = &write_wrs[i];
    //   }
    // }
    // write_wrs[kDoorbellNum - 1].next = nullptr;

    // verbs_util::PostSend(setup.local_qp, write_wrs[0]);
    // for(int i = 0; i < kDoorbellNum; ++i) {
    //   ASSERT_OK_AND_ASSIGN(ibv_wc completion,
    //                       verbs_util::WaitForCompletion(setup.local_cq));
    //   EXPECT_EQ(completion.status, IBV_WC_SUCCESS);
    // }
  }

  static const char* QpStateToStr(ibv_qp_state state) {
    switch (state)
    {
    case IBV_QPS_RESET:
      return "IBV_QPS_RESET";
    case IBV_QPS_INIT:
      return "IBV_QPS_INIT";
    case IBV_QPS_RTR:
      return "IBV_QPS_RTR";
    case IBV_QPS_RTS:
      return "IBV_QPS_RTS";
    case IBV_QPS_SQD:
      return "IBV_QPS_SQD";
    case IBV_QPS_SQE:
      return "IBV_QPS_SQE";
    case IBV_QPS_ERR:
      return "IBV_QPS_ERR";
    case IBV_QPS_UNKNOWN:
      return "IBV_QPS_UNKNOWN";
    default:
      return "Err";
    }
  }

  void QueryQpState(const char* tips, BasicSetup& setup) {
    if(tips) LOG(INFO) << tips;
    LOG(INFO) << "local_qp state: " << QpStateToStr(GetQpState(setup.local_qp));
    LOG(INFO) << "remote_qp state: " << QpStateToStr(GetQpState(setup.remote_qp));
  }
};

TEST_F(QpErrAndContinueTest, SendMrOtherPdLocalAndContinue) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge rsge = verbs_util::CreateSge(setup.buffer.span(), remote_mr);
  ibv_recv_wr recv =
      verbs_util::CreateRecvWr(/*wr_id=*/0, &rsge, /*num_sge=*/1);
  verbs_util::PostRecv(setup.remote_qp, recv);

  ibv_sge ssge = verbs_util::CreateSge(setup.buffer.span(), local_mr);
  ibv_send_wr send =
      verbs_util::CreateSendWr(/*wr_id=*/1, &ssge, /*num_sge=1*/ 1);
  verbs_util::PostSend(setup.local_qp, send);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR);

  EXPECT_EQ(GetQpState(setup.local_qp), IBV_QPS_ERR);
  EXPECT_EQ(GetQpState(setup.remote_qp), IBV_QPS_RTS);
  // continue
  QueryQpState("Before continue:", setup);
  SendRecvPingPongNormalAfterErr(setup);
  QueryQpState("After continue:", setup);
}

TEST_F(QpErrAndContinueTest, SendMrOtherPdRemoteAndContinue) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge rsge = verbs_util::CreateSge(setup.buffer.span(), remote_mr);
  ibv_recv_wr recv =
      verbs_util::CreateRecvWr(/*wr_id=*/0, &rsge, /*num_sge=*/1);
  verbs_util::PostRecv(setup.remote_qp, recv);

  ibv_sge ssge = verbs_util::CreateSge(setup.buffer.span(), local_mr);
  ibv_send_wr send =
      verbs_util::CreateSendWr(/*wr_id=*/1, &ssge, /*num_sge=1*/ 1);
  verbs_util::PostSend(setup.local_qp, send);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(setup.remote_cq));
  EXPECT_THAT(completion.status,
              AnyOf(IBV_WC_LOC_PROT_ERR, IBV_WC_LOC_QP_OP_ERR));
  ASSERT_OK_AND_ASSIGN(completion,
                       verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_REM_OP_ERR);

  EXPECT_EQ(GetQpState(setup.local_qp), IBV_QPS_ERR);
  EXPECT_EQ(GetQpState(setup.remote_qp), IBV_QPS_ERR);
  // continue
  QueryQpState("Before continue:", setup);
  SendRecvPingPongNormalAfterErr(setup);
  QueryQpState("After continue:", setup);
}

TEST_F(QpErrAndContinueTest, BasicReadMrOtherPdLocalAndContinue) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge sg = verbs_util::CreateSge(setup.buffer.span(), local_mr);
  ibv_send_wr read = verbs_util::CreateReadWr(
      /*wr_id=*/1, &sg, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey);
  verbs_util::PostSend(setup.local_qp, read);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR);

  EXPECT_EQ(GetQpState(setup.local_qp), IBV_QPS_ERR);
  EXPECT_EQ(GetQpState(setup.remote_qp), IBV_QPS_RTS);
  // continue
  QueryQpState("Before continue:", setup);
  SendRecvPingPongNormalAfterErr(setup);
  QueryQpState("After continue:", setup);
}

TEST_F(QpErrAndContinueTest, BasicReadMrOtherPdRemoteAndContinue) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge sg = verbs_util::CreateSge(setup.buffer.span(), local_mr);
  ibv_send_wr read = verbs_util::CreateReadWr(
      /*wr_id=*/1, &sg, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey);
  verbs_util::PostSend(setup.local_qp, read);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_REM_ACCESS_ERR);

  EXPECT_EQ(GetQpState(setup.local_qp), IBV_QPS_ERR);
  EXPECT_EQ(GetQpState(setup.remote_qp), IBV_QPS_ERR);
  // continue
  QueryQpState("Before continue:", setup);
  SendRecvPingPongNormalAfterErr(setup);
  QueryQpState("After continue:", setup);
}

TEST_F(QpErrAndContinueTest, BasicWriteMrOtherPdLocalAndContinue) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge sg = verbs_util::CreateSge(setup.buffer.span(), local_mr);
  ibv_send_wr write = verbs_util::CreateWriteWr(
      /*wr_id=*/1, &sg, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey);
  verbs_util::PostSend(setup.local_qp, write);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR);

  EXPECT_EQ(GetQpState(setup.local_qp), IBV_QPS_ERR);
  EXPECT_EQ(GetQpState(setup.remote_qp), IBV_QPS_RTS);
  // continue
  QueryQpState("Before continue:", setup);
  SendRecvPingPongNormalAfterErr(setup);
  QueryQpState("After continue:", setup);
}

TEST_F(QpErrAndContinueTest, BasicWriteMrOtherPdRemoteAndContinue) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge sg = verbs_util::CreateSge(setup.buffer.span(), local_mr);
  ibv_send_wr write = verbs_util::CreateWriteWr(
      /*wr_id=*/1, &sg, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey);
  verbs_util::PostSend(setup.local_qp, write);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_REM_ACCESS_ERR);

  EXPECT_EQ(GetQpState(setup.local_qp), IBV_QPS_ERR);
  EXPECT_EQ(GetQpState(setup.remote_qp), IBV_QPS_ERR);
  // continue
  QueryQpState("Before continue:", setup);
  SendRecvPingPongNormalAfterErr(setup);
  QueryQpState("After continue:", setup);
}

TEST_F(QpErrAndContinueTest, BasicFetchAddMrOtherPdLocalAndContinue) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge sg = verbs_util::CreateSge(setup.buffer.span(), local_mr);
  sg.length = 8;
  ibv_send_wr fetch_add = verbs_util::CreateFetchAddWr(
      /*wr_id=*/1, &sg, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey,
      kCompareAdd);
  verbs_util::PostSend(setup.local_qp, fetch_add);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR);

  EXPECT_EQ(GetQpState(setup.local_qp), IBV_QPS_ERR);
  EXPECT_EQ(GetQpState(setup.remote_qp), IBV_QPS_RTS);
  // continue
  QueryQpState("Before continue:", setup);
  SendRecvPingPongNormalAfterErr(setup);
  QueryQpState("After continue:", setup);
}

TEST_F(QpErrAndContinueTest, BasicFetchAddMrOtherPdRemoteAndContinue) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge sg = verbs_util::CreateSge(setup.buffer.span(), local_mr);
  sg.length = 8;
  ibv_send_wr fetch_add = verbs_util::CreateFetchAddWr(
      /*wr_id=*/1, &sg, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey,
      kCompareAdd);
  verbs_util::PostSend(setup.local_qp, fetch_add);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_REM_ACCESS_ERR);

  EXPECT_EQ(GetQpState(setup.local_qp), IBV_QPS_ERR);
  EXPECT_EQ(GetQpState(setup.remote_qp), IBV_QPS_ERR);
  // continue
  QueryQpState("Before continue:", setup);
  SendRecvPingPongNormalAfterErr(setup);
  QueryQpState("After continue:", setup);
}

TEST_F(QpErrAndContinueTest, BasicCompSwapMrOtherPdLocalAndContinue) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge sg = verbs_util::CreateSge(setup.buffer.span(), local_mr);
  sg.length = 8;
  ibv_send_wr comp_swap = verbs_util::CreateCompSwapWr(
      /*wr_id=*/1, &sg, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey,
      kCompareAdd, kSwap);
  verbs_util::PostSend(setup.local_qp, comp_swap);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_LOC_PROT_ERR);

  EXPECT_EQ(GetQpState(setup.local_qp), IBV_QPS_ERR);
  EXPECT_EQ(GetQpState(setup.remote_qp), IBV_QPS_RTS);
  // continue
  QueryQpState("Before continue:", setup);
  SendRecvPingPongNormalAfterErr(setup);
  QueryQpState("After continue:", setup);
}

TEST_F(QpErrAndContinueTest, BasicCompSwapMrOtherPdRemoteAndContinue) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.other_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());

  ibv_sge sg = verbs_util::CreateSge(setup.buffer.span(), local_mr);
  sg.length = 8;
  ibv_send_wr comp_swap = verbs_util::CreateCompSwapWr(
      /*wr_id=*/1, &sg, /*num_sge=*/1, setup.buffer.data(), remote_mr->rkey,
      kCompareAdd, kSwap);
  verbs_util::PostSend(setup.local_qp, comp_swap);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_REM_ACCESS_ERR);

  EXPECT_EQ(GetQpState(setup.local_qp), IBV_QPS_ERR);
  EXPECT_EQ(GetQpState(setup.remote_qp), IBV_QPS_ERR);
  // continue
  QueryQpState("Before continue:", setup);
  SendRecvPingPongNormalAfterErr(setup);
  QueryQpState("After continue:", setup);
}

}  // namespace rdma_unit_test
