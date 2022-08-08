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

namespace rdma_unit_test {

using ::testing::AnyOf;
using ::testing::IsNull;
using ::testing::NotNull;
using verbs_util::GetQpState;

class CornerCaseTest : public RdmaVerbsFixture {
 protected:
  static constexpr uint32_t kClientMemoryPages = 2;
  static constexpr uint32_t kSendMaxSgeNum = 6;

  struct BasicSetup {
    RdmaMemBlock buffer;
    ibv_context* context;
    ibv_device_attr device_attr;
    PortAttribute port_attr;
    ibv_cq* local_cq;
    ibv_cq* remote_cq;
    ibv_pd* qp_pd;
    ibv_qp* local_qp;
    ibv_qp* remote_qp;
  };

  absl::StatusOr<BasicSetup> CreateBasicSetup() {
    BasicSetup setup;
    setup.buffer = ibv_.AllocBuffer(kClientMemoryPages);
    ASSIGN_OR_RETURN(setup.context, ibv_.OpenDevice());
    setup.port_attr = ibv_.GetPortAttribute(setup.context);

    int ret = ibv_query_device(setup.context, &setup.device_attr);
    EXPECT_EQ(ret, 0);
    LOG(INFO) << "max_sge: " << setup.device_attr.max_sge;
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
    setup.local_qp = ibv_.CreateQp(setup.qp_pd, setup.local_cq, IBV_QPT_RC, QpInitAttribute().set_max_send_sge(kSendMaxSgeNum));
    if (!setup.local_qp) {
      return absl::InternalError("Failed to create local qp.");
    }
    setup.remote_qp = ibv_.CreateQp(setup.qp_pd, setup.remote_cq, IBV_QPT_RC, QpInitAttribute().set_max_send_sge(kSendMaxSgeNum));
    if (!setup.remote_qp) {
      return absl::InternalError("Failed to create remote qp.");
    }
    RETURN_IF_ERROR(ibv_.SetUpLoopbackRcQps(setup.local_qp, setup.remote_qp,
                                            setup.port_attr));
    return setup;
  }
};

TEST_F(CornerCaseTest, MaxSendSgeAndCrossPageTest) {
  ASSERT_OK_AND_ASSIGN(BasicSetup setup, CreateBasicSetup());
  ibv_mr* local_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(local_mr, NotNull());
  ibv_mr* remote_mr = ibv_.RegMr(setup.qp_pd, setup.buffer);
  ASSERT_THAT(remote_mr, NotNull());
  uint64_t wr_id_num = 0;
  constexpr size_t kSizePerSge = 16; // 16B

  memset(setup.buffer.data(), 0, kPageSize*kClientMemoryPages);
  *(uint64_t*)(setup.buffer.data()+kPageSize-kSizePerSge/2) = 3;
  *(uint64_t*)(setup.buffer.data()+kPageSize) = 4;
  ibv_sge remote_sg = verbs_util::CreateSge(setup.buffer.span(), remote_mr);
  ibv_sge local_cross_page_sg = verbs_util::CreateSge(
      setup.buffer.subspan(kPageSize-kSizePerSge/2, kSizePerSge), local_mr);

  ibv_recv_wr recv_wr = verbs_util::CreateRecvWr(
        wr_id_num++, &remote_sg, /*num_sge=*/1);
  verbs_util::PostRecv(setup.remote_qp, recv_wr);

  ibv_sge send_sges[kSendMaxSgeNum];
  for(uint32_t i = 0; i < kSendMaxSgeNum; ++i) {
    send_sges[i] = local_cross_page_sg;
  }
  ibv_send_wr send_wr = verbs_util::CreateSendWr(
        wr_id_num++, send_sges, /*num_sge=*/kSendMaxSgeNum);
  verbs_util::PostSend(setup.local_qp, send_wr);

  wr_id_num = 0;
  ASSERT_OK_AND_ASSIGN(ibv_wc completion,
                        verbs_util::WaitForCompletion(setup.remote_cq));
  EXPECT_EQ(completion.status, IBV_WC_SUCCESS); // IBV_WC_WR_FLUSH_ERR
  EXPECT_EQ(completion.wr_id, wr_id_num++);

  ASSERT_OK_AND_ASSIGN(completion,
                        verbs_util::WaitForCompletion(setup.local_cq));
  EXPECT_EQ(completion.status, IBV_WC_SUCCESS); // IBV_WC_WR_FLUSH_ERR
  EXPECT_EQ(completion.wr_id, wr_id_num++);
  for(uint32_t i = 0; i < kSendMaxSgeNum; ++i) {
    uint64_t* ptr = (uint64_t*)(setup.buffer.data())+i*2;
    EXPECT_EQ(ptr[0], 3);
    EXPECT_EQ(ptr[1], 4);
  }
}

// 添加 测试sq size 的样例

}  // namespace rdma_unit_test
