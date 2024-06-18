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
// This test only covers specific behavior of the QExpr-operator. See also a
// full coverage test in
// py/arolla/operator_tests/strings_format_test.py.

#include "arolla/qexpr/operators/strings/format.h"

#include <cstdint>
#include <type_traits>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "arolla/memory/optional_value.h"
#include "arolla/qexpr/operators.h"
#include "arolla/qtype/base_types.h"
#include "arolla/util/bytes.h"
#include "arolla/util/testing/status_matchers_backport.h"
#include "arolla/util/text.h"

namespace arolla {

using ::arolla::testing::IsOkAndHolds;
using ::arolla::testing::StatusIs;
using ::testing::HasSubstr;

namespace {

template <typename EvalAsFunctor>
class FormatTest : public ::testing::Test {
 public:
  template <typename... Args>
  absl::StatusOr<Bytes> InvokeOperator(const Args&... args) {
    if constexpr (EvalAsFunctor::value) {
      auto result = FormatOperatorFamily{}(args...);
      static_assert(std::is_same_v<decltype(result), absl::StatusOr<Bytes>>);
      return result;
    } else {
      return ::arolla::InvokeOperator<Bytes>("strings._format_bytes", args...);
    }
  }

  template <typename... Args>
  absl::StatusOr<OptionalValue<Bytes>> InvokeOperatorOptional(
      const Args&... args) {
    if constexpr (EvalAsFunctor::value) {
      auto result = FormatOperatorFamily{}(args...);
      static_assert(std::is_same_v<decltype(result),
                                   absl::StatusOr<OptionalValue<Bytes>>>);
      return result;
    } else {
      return ::arolla::InvokeOperator<OptionalValue<Bytes>>(
          "strings._format_bytes", args...);
    }
  }
};

TYPED_TEST_SUITE_P(FormatTest);

TYPED_TEST_P(FormatTest, FormatFloats) {
  Bytes format_spec("a=%0.2f b=%0.3f");

  // Format float types.
  float a = 20.5f;
  double b = 3.75;
  EXPECT_THAT(this->InvokeOperator(format_spec, a, b),
              IsOkAndHolds(Bytes("a=20.50 b=3.750")));
}

TYPED_TEST_P(FormatTest, FormatIntegers) {
  Bytes format_spec("c=%02d, d=%d");

  // Format integers.
  int32_t c = 3;
  int64_t d = 4;
  EXPECT_THAT(this->InvokeOperator(format_spec, c, d),
              IsOkAndHolds(Bytes("c=03, d=4")));
}

TYPED_TEST_P(FormatTest, FormatText) {
  Bytes format_spec("%s is %d years older than %s.");
  EXPECT_THAT(
      this->InvokeOperator(format_spec, Bytes("Sophie"), 2, Bytes("Katie")),
      IsOkAndHolds(Bytes("Sophie is 2 years older than Katie.")));
}

TYPED_TEST_P(FormatTest, FormatOptional) {
  Bytes format_spec("The atomic weight of %s is %0.3f");
  // All values present
  EXPECT_THAT(
      this->InvokeOperatorOptional(format_spec, OptionalValue<Bytes>("Iron"),
                                   OptionalValue<float>(55.845)),
      IsOkAndHolds(
          OptionalValue<Bytes>("The atomic weight of Iron is 55.845")));
  EXPECT_THAT(this->InvokeOperatorOptional(OptionalValue<Bytes>(format_spec),
                                           OptionalValue<Bytes>("Iron"),
                                           OptionalValue<float>(55.845)),
              IsOkAndHolds(
                  OptionalValue<Bytes>("The atomic weight of Iron is 55.845")));

  // One or more values missing
  EXPECT_THAT(this->InvokeOperatorOptional(format_spec,
                                           OptionalValue<Bytes>("Unobtainium"),
                                           OptionalValue<float>{}),
              IsOkAndHolds(OptionalValue<Bytes>{}));
  EXPECT_THAT(this->InvokeOperatorOptional(OptionalValue<Bytes>(),
                                           OptionalValue<Bytes>("Unobtainium"),
                                           OptionalValue<float>{0}),
              IsOkAndHolds(OptionalValue<Bytes>{}));
}

TYPED_TEST_P(FormatTest, FormatMismatchedTypes) {
  Bytes format_spec("%s's atomic weight is %f");
  EXPECT_THAT(this->InvokeOperator(format_spec, 1.0079, Bytes("Hydrogen")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("doesn't match format arguments")));
}

TYPED_TEST_P(FormatTest, FormatUnsupportedType) {
  Bytes format_spec("Payload is %s.");
  EXPECT_THAT(
      this->InvokeOperator(format_spec, Text("abc")),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("TEXT is not a supported format argument type")));
}

REGISTER_TYPED_TEST_SUITE_P(FormatTest, FormatFloats, FormatIntegers,
                            FormatText, FormatOptional, FormatMismatchedTypes,
                            FormatUnsupportedType);

INSTANTIATE_TYPED_TEST_SUITE_P(Operator, FormatTest, std::bool_constant<false>);
INSTANTIATE_TYPED_TEST_SUITE_P(Functor, FormatTest, std::bool_constant<true>);

}  // namespace
}  // namespace arolla
