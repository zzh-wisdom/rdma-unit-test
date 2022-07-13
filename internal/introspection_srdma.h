/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef THIRD_PARTY_RDMA_UNIT_TEST_INTERNAL_INTROSPECTION_SRDMA_H_
#define THIRD_PARTY_RDMA_UNIT_TEST_INTERNAL_INTROSPECTION_SRDMA_H_

#include <string>

#include "absl/container/flat_hash_map.h"
#include "infiniband/verbs.h"
#include "internal/introspection_registrar.h"
#include "public/introspection.h"

namespace rdma_unit_test {

// Concrete class to override specific behaviour for SRDMA.
class IntrospectionSrdma : public NicIntrospection {
 public:
  // Register srdma NIC with the Introspection Registrar.
  static void Register() {
    IntrospectionRegistrar::GetInstance().Register(
        "srdma", [](const std::string& name, const ibv_device_attr& attr) {
          return new IntrospectionSrdma(name, attr);
        });
  }

  bool SupportsIpV6() const override { return false; }

  bool SupportsRcQp() const override { return false; }

  // Provider does not support cq_ex.
  bool SupportsExtendedCqs() const override { return false; }

 protected:
  const absl::flat_hash_map<TestcaseKey, std::string>& GetDeviations()
      const override {
    static const absl::flat_hash_map<TestcaseKey, std::string> deviations{
        {{"DeviceLimitTest", "MaxMw"}, "Provider does not support MW."},
        {{"PdBindTest", "MwOnOtherPd"}, "Provider does not support MW."},
        {{"PdBindTest", "MrOnOtherPd"}, "Provider does not support MW."},
        {{"PdBindTest", "MrMwOnOtherPd"}, "Provider does not support MW."},
        {{"PdSrqTest", "CreateSrq"}, "Provider does not support srq."},
        {{"PdSrqTest", "SrqRecvMrSrqMatch"}, "Provider does not support srq."},
        {{"PdSrqTest", "SrqRecvMrSrqMismatch"}, "Provider does not support srq."},
        {{"PdType1MwTest", "ReadMwOtherPd"}, "Provider does not support MW."},
        {{"PdType1MwTest", "WriteMwOtherPd"}, "Provider does not support MW."},
        {{"PdType1MwTest", "FetchAddMwOtherPd"}, "Provider does not support MW."},
        {{"PdType1MwTest", "CompSwapMwOtherPd"}, "Provider does not support MW."},
        {{"PdUdLoopbackTest", "SendAhOnOtherPd"}, "Provider does not support UD connection."},
        // Returns success completion.
        {{"BufferTest", "ZeroByteReadInvalidRKey"}, ""},
        // Zero byte write is successful.
        {{"BufferTest", "ZeroByteWriteInvalidRKey"}, ""},
        // Hardware returns true when requesting notification on a CQ without a
        // Completion Channel.
        // {{"CompChannelTest", "RequestNotificationOnCqWithoutCompChannel"}, ""},
        // {{"CompChannelTest", "AcknowledgeWithoutOutstanding"},
        //  "Provider crashes when ack-ing without outstanding completion."},
        // TODO(author2): Be more specific.
        {{"QpTest", "OverflowSendWr"}, "Does not handle overflow QP."},
        {{"QpTest", "UnknownType"}, "Can create QPs of unknown type."},
        // Does not handle overflow well.
        {{"SrqTest", "OverflowSrq"}, ""},
    };
    return deviations;
  }

 private:
  IntrospectionSrdma() = delete;
  ~IntrospectionSrdma() = default;
  IntrospectionSrdma(const std::string& name, const ibv_device_attr& attr)
      : NicIntrospection(name, attr) {}
};

}  // namespace rdma_unit_test

#endif  // THIRD_PARTY_RDMA_UNIT_TEST_INTERNAL_INTROSPECTION_SRDMA_H_
