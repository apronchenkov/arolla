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
// A basic test for serialization/deserialization facility.

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "arolla/array/array.h"
#include "arolla/array/qtype/types.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/dense_array/qtype/types.h"
#include "arolla/expr/expr.h"
#include "arolla/expr/expr_node.h"
#include "arolla/expr/expr_operator.h"
#include "arolla/expr/expr_operator_signature.h"
#include "arolla/expr/lambda_expr_operator.h"
#include "arolla/expr/testing/testing.h"
#include "arolla/expr/tuple_expr_operator.h"
#include "arolla/memory/optional_value.h"
#include "arolla/qtype/base_types.h"
#include "arolla/qtype/optional_qtype.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/qtype/tuple_qtype.h"
#include "arolla/qtype/typed_value.h"
#include "arolla/qtype/weak_qtype.h"
#include "arolla/serialization/decode.h"
#include "arolla/serialization/encode.h"
#include "arolla/util/bytes.h"
#include "arolla/util/init_arolla.h"
#include "arolla/util/testing/status_matchers_backport.h"
#include "arolla/util/status_macros_backport.h"

namespace arolla::serialization {
namespace {

using ::arolla::expr::ExprOperatorPtr;
using ::arolla::expr::MakeTupleOperator;
using ::arolla::testing::EqualsExpr;
using ::arolla::testing::IsOkAndHolds;
using ::arolla::testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::Truly;

auto EqualsTypedValue(const TypedValue& expected_value) {
  return Truly([expected_value](const TypedValue& actual_value) {
    return expected_value.GetFingerprint() == actual_value.GetFingerprint();
  });
}

// Returns a value.
absl::StatusOr<TypedValue> GenValue() {
  // Use simple values with deterministic fingerprints:
  return MakeTupleFromFields(
      // scalar
      GetQType<float>(), float{3.14},   //
      GetQType<Bytes>(), Bytes("foo"),  //
      GetWeakFloatQType(),
      TypedValue::FromValueWithQType(1., GetWeakFloatQType()).value(),
      // optional
      GetOptionalQType<float>(), OptionalValue<float>(3.14),  //
      GetOptionalQType<Bytes>(), OptionalValue<Bytes>(Bytes("foo")),
      GetOptionalWeakFloatQType(),
      TypedValue::FromValueWithQType(OptionalValue<double>(1.),
                                     GetOptionalWeakFloatQType())
          .value(),
      // tuple
      MakeTupleQType({GetQType<float>(), GetQType<Bytes>()}),  //
      MakeTupleFromFields(float{3.14}, Bytes("foo")),
      // operator
      GetQType<ExprOperatorPtr>(), MakeTupleOperator::Make(),
      // dense_array
      GetDenseArrayQType<float>(),
      CreateConstDenseArray<float>(10, float{3.14}),  //
      GetDenseArrayQType<Bytes>(), CreateConstDenseArray<Bytes>(10, "foo"),
      // array
      GetArrayQType<float>(), Array<float>(10, float{3.14}),  //
      GetArrayQType<Bytes>(), Array<Bytes>(10, Bytes("foo")));
}

// Returns a big expression.
absl::StatusOr<expr::ExprNodePtr> GenExpr() {
  auto a = expr::Placeholder("a");
  auto b = expr::Placeholder("b");
  auto c = expr::Placeholder("c");

  ASSIGN_OR_RETURN(
      auto d, expr::CallOp(
                  "math.pow",
                  {expr::CallOp(
                       "math.subtract",
                       {expr::CallOp("math.multiply", {b, b}),
                        expr::CallOp("math.multiply",
                                     {expr::Literal(4.0f),
                                      expr::CallOp("math.multiply", {a, c})})}),
                   expr::Literal(0.5f)}));
  ASSIGN_OR_RETURN(
      auto x0,
      expr::CallOp(
          "math.divide",
          {expr::CallOp(
               "math.subtract",
               {expr::CallOp("math.multiply", {expr::Literal(-1.0f), b}), d}),
           expr::CallOp("math.multiply", {expr::Literal(2.0f), a})}));
  ASSIGN_OR_RETURN(
      auto x1,
      expr::CallOp(
          "math.divide",
          {expr::CallOp(
               "math.add",
               {expr::CallOp("math.multiply", {expr::Literal(-1.0f), b}), d}),
           expr::CallOp("math.multiply", {expr::Literal(2.0f), a})}));

  ASSIGN_OR_RETURN(
      auto op,
      expr::MakeLambdaOperator("solve_quadratic_equation",
                               expr::ExprOperatorSignature{{"a"}, {"b"}, {"c"}},
                               expr::CallOp("core.make_tuple", {x0, x1})));
  return expr::CallOp(op,
                      {expr::Literal(1.0f), expr::Leaf("p"), expr::Leaf("q")});
}

class SerializationTest : public ::testing::Test {
  void SetUp() override { ASSERT_OK(InitArolla()); }
};

TEST_F(SerializationTest, Basic) {
  ASSERT_OK_AND_ASSIGN(auto value, GenValue());
  ASSERT_OK_AND_ASSIGN(auto expr, GenExpr());
  ASSERT_OK_AND_ASSIGN(auto container_proto, Encode({value}, {expr}));
  ASSERT_OK_AND_ASSIGN(auto decode_result, Decode(container_proto));
  EXPECT_THAT(decode_result.values, ElementsAre(EqualsTypedValue(value)));
  EXPECT_THAT(decode_result.exprs, ElementsAre(EqualsExpr(expr)));
}

TEST_F(SerializationTest, DecodeExpr) {
  ASSERT_OK_AND_ASSIGN(auto value, GenValue());
  ASSERT_OK_AND_ASSIGN(auto expr, GenExpr());
  {
    ASSERT_OK_AND_ASSIGN(auto container_proto, Encode({}, {expr}));
    EXPECT_THAT(DecodeExpr(container_proto), IsOkAndHolds(EqualsExpr(expr)));
  }
  {
    ASSERT_OK_AND_ASSIGN(auto container_proto, Encode({value}, {expr}));
    EXPECT_THAT(DecodeExpr(container_proto),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         "unable to decode expression: expected 1 expression "
                         "and 0 values in the container, got 1 and 1"));
  }
}

TEST_F(SerializationTest, DecodeValue) {
  ASSERT_OK_AND_ASSIGN(auto value, GenValue());
  ASSERT_OK_AND_ASSIGN(auto expr, GenExpr());
  {
    ASSERT_OK_AND_ASSIGN(auto container_proto, Encode({value}, {}));
    EXPECT_THAT(DecodeValue(container_proto),
                IsOkAndHolds(EqualsTypedValue(value)));
  }
  {
    ASSERT_OK_AND_ASSIGN(auto container_proto, Encode({value}, {expr}));
    EXPECT_THAT(DecodeValue(container_proto),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         "unable to decode value: expected 1 value "
                         "and 0 expressions in the container, got 1 and 1"));
  }
}

}  // namespace
}  // namespace arolla::serialization
