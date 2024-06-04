// Copyright 2024 Google LLC
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
//
#include "arolla/serialization_base/container_proto.h"

#include <cstdint>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "arolla/serialization_base/base.pb.h"
#include "arolla/serialization_base/container.h"
#include "arolla/util/testing/equals_proto.h"
#include "arolla/util/testing/status_matchers_backport.h"

namespace arolla::serialization_base {
namespace {

using ::arolla::testing::EqualsProto;
using ::arolla::testing::IsOk;
using ::arolla::testing::IsOkAndHolds;
using ::arolla::testing::StatusIs;
using ::testing::HasSubstr;
using ::testing::InSequence;
using ::testing::Return;

TEST(ContainerProtoBuilderTest, TrivialBehaviour) {
  ContainerProtoBuilder container_builder;
  {
    DecodingStepProto decoding_step_proto;
    decoding_step_proto.mutable_codec()->set_name("codec1");
    ASSERT_THAT(container_builder.Add(std::move(decoding_step_proto)),
                IsOkAndHolds(0));
  }
  {
    DecodingStepProto decoding_step_proto;
    decoding_step_proto.mutable_leaf_node()->set_leaf_key("key1");
    ASSERT_THAT(container_builder.Add(std::move(decoding_step_proto)),
                IsOkAndHolds(0));
  }
  {
    DecodingStepProto decoding_step_proto;
    decoding_step_proto.set_output_expr_index(0);
    ASSERT_THAT(container_builder.Add(std::move(decoding_step_proto)),
                IsOkAndHolds(1));
  }
  {
    DecodingStepProto decoding_step_proto;
    decoding_step_proto.mutable_codec()->set_name("codec2");
    ASSERT_THAT(container_builder.Add(std::move(decoding_step_proto)),
                IsOkAndHolds(1));
  }
  {
    DecodingStepProto decoding_step_proto;
    decoding_step_proto.mutable_placeholder_node()->set_placeholder_key("key2");
    ASSERT_THAT(container_builder.Add(std::move(decoding_step_proto)),
                IsOkAndHolds(1));
  }
  {
    DecodingStepProto decoding_step_proto;
    decoding_step_proto.mutable_value();
    ASSERT_THAT(container_builder.Add(std::move(decoding_step_proto)),
                IsOkAndHolds(2));
  }
  {
    DecodingStepProto decoding_step_proto;
    decoding_step_proto.set_output_expr_index(1);
    ASSERT_THAT(container_builder.Add(std::move(decoding_step_proto)),
                IsOkAndHolds(2));
  }
  {
    DecodingStepProto decoding_step_proto;
    decoding_step_proto.set_output_value_index(2);
    ASSERT_THAT(container_builder.Add(std::move(decoding_step_proto)),
                IsOkAndHolds(1));
  }
  EXPECT_TRUE(EqualsProto(
      std::move(container_builder).Finish(),
      R"pb(
        version: 1
        codecs { name: "codec1" }
        codecs { name: "codec2" }
        decoding_steps { leaf_node { leaf_key: "key1" } }
        decoding_steps { placeholder_node { placeholder_key: "key2" } }
        decoding_steps { value {} }
        output_value_indices: [ 2 ]
        output_expr_indices: [ 0, 1 ]
      )pb"));
}

class MockContainerProcessor : public ContainerProcessor {
 public:
  MOCK_METHOD(absl::Status, OnDecodingStep,
              (uint64_t, const DecodingStepProto& decoding_step_proto),
              (override));
};

TEST(ProcessContainerProto, TrivialBehaviour) {
  ContainerProto container_proto;
  container_proto.set_version(1);
  container_proto.add_codecs()->set_name("codec1");
  container_proto.add_codecs()->set_name("codec2");
  container_proto.add_decoding_steps()->mutable_leaf_node()->set_leaf_key(
      "key1");
  container_proto.add_decoding_steps()
      ->mutable_placeholder_node()
      ->set_placeholder_key("key2");
  container_proto.add_decoding_steps()->mutable_value();
  container_proto.add_output_value_indices(2);
  container_proto.add_output_expr_indices(0);
  container_proto.add_output_expr_indices(1);
  MockContainerProcessor mock_container_processor;
  {
    InSequence seq;
    EXPECT_CALL(
        mock_container_processor,
        OnDecodingStep(0, EqualsProto(R"pb(codec: { name: "codec1" })pb")));
    EXPECT_CALL(
        mock_container_processor,
        OnDecodingStep(1, EqualsProto(R"pb(codec: { name: "codec2" })pb")));
    EXPECT_CALL(mock_container_processor,
                OnDecodingStep(
                    0, EqualsProto(R"pb(leaf_node: { leaf_key: "key1" })pb")));
    EXPECT_CALL(mock_container_processor,
                OnDecodingStep(1, EqualsProto(R"pb(placeholder_node: {
                                                     placeholder_key: "key2"
                                                   })pb")));
    EXPECT_CALL(mock_container_processor,
                OnDecodingStep(2, EqualsProto(R"pb(value: {})pb")));
    EXPECT_CALL(mock_container_processor,
                OnDecodingStep(0, EqualsProto(R"pb(output_value_index: 2)pb")));
    EXPECT_CALL(mock_container_processor,
                OnDecodingStep(0, EqualsProto(R"pb(output_expr_index: 0)pb")));
    EXPECT_CALL(mock_container_processor,
                OnDecodingStep(0, EqualsProto(R"pb(output_expr_index: 1)pb")));
  }
  EXPECT_THAT(ProcessContainerProto(container_proto, mock_container_processor),
              IsOk());
}

TEST(ProcessContainerProto, MissingContainerVersion) {
  ContainerProto container_proto;
  MockContainerProcessor mock_container_processor;
  EXPECT_THAT(ProcessContainerProto(container_proto, mock_container_processor),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("missing container.version")));
}

TEST(ProcessContainerProto, WrongContainerVersion) {
  ContainerProto container_proto;
  container_proto.set_version(100);
  MockContainerProcessor mock_container_processor;
  EXPECT_THAT(
      ProcessContainerProto(container_proto, mock_container_processor),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("expected container.version to be 1, got 100")));
}

TEST(ProcessContainerProto, ProcessorFailureOnCodec) {
  ContainerProto container_proto;
  container_proto.set_version(1);
  container_proto.add_codecs()->set_name("codec1");
  container_proto.add_codecs()->set_name("codec2");
  MockContainerProcessor mock_container_processor;
  {
    InSequence seq;
    EXPECT_CALL(
        mock_container_processor,
        OnDecodingStep(0, EqualsProto(R"pb(codec: { name: "codec1" })pb")));
    EXPECT_CALL(
        mock_container_processor,
        OnDecodingStep(1, EqualsProto(R"pb(codec: { name: "codec2" })pb")))
        .WillOnce(Return(absl::FailedPreconditionError("stop")));
  }
  EXPECT_THAT(ProcessContainerProto(container_proto, mock_container_processor),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("stop; while handling codecs[1]")));
}

TEST(ProcessContainerProto, ProcessorFailureOnDecodingStep) {
  ContainerProto container_proto;
  container_proto.set_version(1);
  container_proto.add_decoding_steps()->mutable_leaf_node()->set_leaf_key(
      "key1");
  container_proto.add_decoding_steps()->mutable_value();
  MockContainerProcessor mock_container_processor;
  {
    InSequence seq;
    EXPECT_CALL(mock_container_processor,
                OnDecodingStep(
                    0, EqualsProto(R"pb(leaf_node: { leaf_key: "key1" })pb")));
    EXPECT_CALL(mock_container_processor,
                OnDecodingStep(1, EqualsProto(R"pb(value {})pb")))
        .WillOnce(Return(absl::FailedPreconditionError("stop")));
  }
  EXPECT_THAT(ProcessContainerProto(container_proto, mock_container_processor),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("stop; while handling decoding_steps[1]")));
}

TEST(ProcessContainerProto, ProcessorFailureOnOutputValueIndex) {
  ContainerProto container_proto;
  container_proto.set_version(1);
  container_proto.add_output_value_indices(1);
  MockContainerProcessor mock_container_processor;
  EXPECT_CALL(mock_container_processor,
              OnDecodingStep(0, EqualsProto(R"pb(output_value_index: 1)pb")))
      .WillOnce(Return(absl::FailedPreconditionError("stop")));
  EXPECT_THAT(
      ProcessContainerProto(container_proto, mock_container_processor),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("stop; while handling output_value_indices[0]")));
}

TEST(ProcessContainerProto, ProcessorFailureOnOutputExprIndex) {
  ContainerProto container_proto;
  container_proto.set_version(1);
  container_proto.add_output_expr_indices(2);
  MockContainerProcessor mock_container_processor;
  EXPECT_CALL(mock_container_processor,
              OnDecodingStep(0, EqualsProto(R"pb(output_expr_index: 2)pb")))
      .WillOnce(Return(absl::FailedPreconditionError("stop")));
  EXPECT_THAT(
      ProcessContainerProto(container_proto, mock_container_processor),
      StatusIs(absl::StatusCode::kFailedPrecondition,
               HasSubstr("stop; while handling output_expr_indices[0]")));
}

}  // namespace
}  // namespace arolla::serialization_base
