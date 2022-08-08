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
#include <sched.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "infiniband/verbs.h"
#include "internal/verbs_attribute.h"
#include "public/introspection.h"
#include "public/rdma_memblock.h"
#include "public/status_matchers.h"
#include "public/verbs_helper_suite.h"
#include "public/verbs_util.h"
#include "unit/loopback_fixture.h"

namespace rdma_unit_test {

// TODO(author1): Add tests stressing SGEs.
// TODO(author1): Send with insufficient recv buffering (RC and UD).
// TODO(author2): Add QP error state check for relevant testcases.

using ::testing::AnyOf;
using ::testing::Each;
using ::testing::NotNull;

class LoopbackRcMixVerbsTest : public LoopbackFixture {
 public:
  static constexpr char kLocalBufferContent = 'a';
  static constexpr char kRemoteBufferContent = 'b';
  static constexpr int kLargePayloadPages = 128;
  static constexpr int kPages = 1;

  void SetUp() override {
    LoopbackFixture::SetUp();
    if (!Introspection().SupportsRcQp()) {
      GTEST_SKIP() << "Nic does not support RC QP";
    }
  }

 protected:
  absl::StatusOr<std::pair<Client, Client>> CreateConnectedClientsPair(
      int pages = kPages, QpInitAttribute qp_init_attr = QpInitAttribute(),
      QpAttribute qp_attr = QpAttribute()) {
    ASSIGN_OR_RETURN(Client local,
                     CreateClient(IBV_QPT_RC, pages, qp_init_attr));
    std::fill_n(local.buffer.data(), local.buffer.size(), kLocalBufferContent);
    ASSIGN_OR_RETURN(Client remote,
                     CreateClient(IBV_QPT_RC, pages, qp_init_attr));
    std::fill_n(remote.buffer.data(), remote.buffer.size(),
                kRemoteBufferContent);
    RETURN_IF_ERROR(ibv_.ModifyRcQpResetToRts(local.qp, local.port_attr,
                                              remote.port_attr.gid,
                                              remote.qp->qp_num, qp_attr));
    RETURN_IF_ERROR(ibv_.ModifyRcQpResetToRts(remote.qp, remote.port_attr,
                                              local.port_attr.gid,
                                              local.qp->qp_num, qp_attr));
    return std::make_pair(local, remote);
  }
};

TEST_F(LoopbackRcMixVerbsTest, WriteAfterLocalInvalidate) {
  Client local, remote;
  ASSERT_OK_AND_ASSIGN(std::tie(local, remote), CreateConnectedClientsPair()); 

  // Bind a Type 2 MW on remote.
  static constexpr uint32_t rkey = 1024;
  ibv_mw* mw = ibv_.AllocMw(remote.pd, IBV_MW_TYPE_2);
  ASSERT_THAT(mw, NotNull());
  ASSERT_THAT(verbs_util::BindType2MwSync(remote.qp, mw, remote.buffer.span(),
                                          rkey, remote.mr),
              IsOkAndHolds(IBV_WC_SUCCESS));
    
  auto ret = verbs_util::LocalInvalidateSync(remote.qp, remote.mr->rkey); 
  EXPECT_EQ(ret.value(), IBV_WC_SUCCESS);
  // ASSERT_THAT(verbs_util::LocalInvalidateSync(remote.qp, remote.mr->rkey),
  //             IsOkAndHolds(IBV_WC_SUCCESS)); 

  // ibv_sge lsge = verbs_util::CreateSge(local.buffer.span(), local.mr);
  // ibv_send_wr write = verbs_util::CreateWriteWr(wr_id++, &lsge, 1, remote.buffer.data(), remote.mr->rkey);
  // verbs_util::PostSend(local.qp, write);
  // ASSERT_OK_AND_ASSIGN(ibv_wc completion, verbs_util::WaitForCompletion(local.cq));
  // EXPECT_EQ(completion.wr_id, write.wr_id);
  // EXPECT_EQ(completion.qp_num, local.qp->qp_num);
  // EXPECT_EQ(completion.status, IBV_WC_REM_ACCESS_ERR);
}

TEST_F(LoopbackRcMixVerbsTest, WriteAfterSendWithInvalidate) {
  Client local, remote;
  ASSERT_OK_AND_ASSIGN(std::tie(local, remote), CreateConnectedClientsPair()); 

  // Bind a Type 2 MW on remote.
  static constexpr uint32_t rkey = 1024;
  ibv_mw* mw = ibv_.AllocMw(remote.pd, IBV_MW_TYPE_2);
  ASSERT_THAT(mw, NotNull());
  ASSERT_THAT(verbs_util::BindType2MwSync(remote.qp, mw, remote.buffer.span(),
                                          rkey, remote.mr),
              IsOkAndHolds(IBV_WC_SUCCESS));

  ibv_sge rsge = verbs_util::CreateSge(remote.buffer.span(), remote.mr);
  ibv_recv_wr recv =
      verbs_util::CreateRecvWr(/*wr_id=*/0, &rsge, /*num_sge=*/1);
  verbs_util::PostRecv(remote.qp, recv);

  ibv_send_wr send_with_inv = verbs_util::CreateSendWithInvalidateWr(1, mw->rkey);
  verbs_util::PostSend(local.qp, send_with_inv);

  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                       verbs_util::WaitForCompletion(local.cq));
  EXPECT_EQ(completion.status, IBV_WC_SUCCESS);
  EXPECT_EQ(completion.opcode, IBV_WC_SEND);
  EXPECT_EQ(completion.qp_num, local.qp->qp_num);
  EXPECT_EQ(completion.wr_id, 1);
  ASSERT_OK_AND_ASSIGN(completion, verbs_util::WaitForCompletion(remote.cq));
  EXPECT_EQ(completion.status, IBV_WC_SUCCESS);
  EXPECT_EQ(completion.opcode, IBV_WC_RECV);
  EXPECT_EQ(completion.qp_num, remote.qp->qp_num);
  EXPECT_EQ(completion.wr_id, 0);
  EXPECT_EQ(completion.wc_flags, IBV_WC_WITH_INV);
  EXPECT_EQ(mw->rkey, completion.invalidated_rkey);

  // ibv_sge lsge = verbs_util::CreateSge(local.buffer.span(), local.mr);
  // ibv_send_wr write = verbs_util::CreateWriteWr(wr_id++, &lsge, 1, remote.buffer.data(), remote.mr->rkey);
  // verbs_util::PostSend(local.qp, write);
  // ASSERT_OK_AND_ASSIGN(ibv_wc completion, verbs_util::WaitForCompletion(local.cq));
  // EXPECT_EQ(completion.wr_id, write.wr_id);
  // EXPECT_EQ(completion.qp_num, local.qp->qp_num);
  // EXPECT_EQ(completion.status, IBV_WC_REM_ACCESS_ERR);
}

// TEST_F(LoopbackRcMixVerbsTest, Send) {
//   Client local, remote;
//   ASSERT_OK_AND_ASSIGN(std::tie(local, remote), CreateConnectedClientsPair());

//   ibv_sge sge = verbs_util::CreateSge(remote.buffer.span(), remote.mr);
//   ibv_recv_wr recv = verbs_util::CreateRecvWr(/*wr_id=*/0, &sge, /*num_sge=*/1);
//   verbs_util::PostRecv(remote.qp, recv);

//   ibv_sge lsge = verbs_util::CreateSge(local.buffer.span(), local.mr);
//   ibv_send_wr send =
//       verbs_util::CreateSendWr(/*wr_id=*/1, &lsge, /*num_sge=*/1);
//   verbs_util::PostSend(local.qp, send);

//   ASSERT_OK_AND_ASSIGN(ibv_wc completion,
//                        verbs_util::WaitForCompletion(local.cq));
//   EXPECT_EQ(completion.status, IBV_WC_SUCCESS);
//   EXPECT_EQ(completion.opcode, IBV_WC_SEND);
//   EXPECT_EQ(completion.qp_num, local.qp->qp_num);
//   EXPECT_EQ(completion.wr_id, 1);
//   ASSERT_OK_AND_ASSIGN(completion, verbs_util::WaitForCompletion(remote.cq));
//   EXPECT_EQ(completion.status, IBV_WC_SUCCESS);
//   EXPECT_EQ(completion.opcode, IBV_WC_RECV);
//   EXPECT_EQ(completion.qp_num, remote.qp->qp_num);
//   EXPECT_EQ(completion.wr_id, 0);
//   EXPECT_EQ(completion.wc_flags, 0);
//   EXPECT_THAT(remote.buffer.span(), Each(kLocalBufferContent));
// }
}  // namespace rdma_unit_test
