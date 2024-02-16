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
#include "arolla/lazy/lazy.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "arolla/qtype/qtype.h"
#include "arolla/qtype/testing/qtype.h"
#include "arolla/qtype/typed_value.h"
#include "arolla/util/fingerprint.h"
#include "arolla/util/init_arolla.h"
#include "arolla/util/repr.h"
#include "arolla/util/testing/status_matchers_backport.h"

namespace arolla {
namespace {

using ::arolla::testing::IsOkAndHolds;
using ::arolla::testing::StatusIs;
using ::arolla::testing::TypedValueWith;

class LazyTest : public ::testing::Test {
  void SetUp() override { ASSERT_OK(InitArolla()); }
};

TEST_F(LazyTest, MakeLazyFromQValue) {
  auto x = MakeLazyFromQValue(TypedValue::FromValue(GetQTypeQType()));
  EXPECT_EQ(x->value_qtype(), GetQTypeQType());
  EXPECT_EQ(Repr(x), "lazy[QTYPE]");
  EXPECT_THAT(x->Get(),
              IsOkAndHolds(TypedValueWith<QTypePtr>(GetQTypeQType())));
}

TEST_F(LazyTest, LazyValueFingerprint) {
  EXPECT_EQ(
      MakeLazyFromQValue(TypedValue::FromValue(GetQTypeQType()))->fingerprint(),
      MakeLazyFromQValue(TypedValue::FromValue(GetQTypeQType()))
          ->fingerprint());
  EXPECT_NE(
      MakeLazyFromQValue(TypedValue::FromValue(GetQTypeQType()))->fingerprint(),
      MakeLazyFromQValue(TypedValue::FromValue(GetNothingQType()))
          ->fingerprint());
}

TEST_F(LazyTest, MakeLazyFromCallable) {
  auto x = MakeLazyFromCallable(
      GetQTypeQType(), [] { return TypedValue::FromValue(GetNothingQType()); });
  EXPECT_EQ(x->value_qtype(), GetQTypeQType());
  EXPECT_EQ(Repr(x), "lazy[QTYPE]");
  EXPECT_THAT(x->Get(),
              IsOkAndHolds(TypedValueWith<QTypePtr>(GetNothingQType())));
}

TEST_F(LazyTest, LazyCallableFingerprint) {
  auto x = MakeLazyFromCallable(
      GetQTypeQType(), [] { return TypedValue::FromValue(GetNothingQType()); });
  auto y = MakeLazyFromCallable(
      GetQTypeQType(), [] { return TypedValue::FromValue(GetNothingQType()); });
  EXPECT_EQ(x->fingerprint(), x->fingerprint());
  EXPECT_NE(x->fingerprint(), y->fingerprint());
}

TEST_F(LazyTest, LazyCallableError) {
  auto x = MakeLazyFromCallable(
      GetQTypeQType(), [] { return absl::InvalidArgumentError("error"); });
  EXPECT_EQ(x->value_qtype(), GetQTypeQType());
  EXPECT_EQ(Repr(x), "lazy[QTYPE]");
  EXPECT_THAT(x->Get(), StatusIs(absl::StatusCode::kInvalidArgument, "error"));
}

TEST_F(LazyTest, Nullptr) {
  LazyPtr x;
  EXPECT_EQ(Repr(x), "lazy[?]{nullptr}");
  EXPECT_EQ(FingerprintHasher("salt").Combine(x).Finish(),
            FingerprintHasher("salt").Combine(x).Finish());
}

}  // namespace
}  // namespace arolla
