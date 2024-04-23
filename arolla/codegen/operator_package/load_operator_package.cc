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
#include "arolla/codegen/operator_package/load_operator_package.h"

#include <set>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "arolla/codegen/operator_package/operator_package.pb.h"
#include "arolla/expr/expr_operator.h"
#include "arolla/expr/registered_expr_operator.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/serialization/decode.h"
#include "arolla/util/status_macros_backport.h"

namespace arolla::operator_package {

using ::arolla::expr::ExprOperatorPtr;
using ::arolla::expr::ExprOperatorRegistry;

absl::Status LoadOperatorPackage(
    const OperatorPackageProto& operator_package_proto) {
  if (operator_package_proto.version() != 1) {
    return absl::InvalidArgumentError(
        absl::StrFormat("expected operator_package_proto.version=1, got %d",
                        operator_package_proto.version()));
  }

  // Check dependencies.
  auto* const operator_registry = ExprOperatorRegistry::GetInstance();
  auto check_registered_operator_presence = [&](absl::string_view name) {
    return operator_registry->LookupOperatorOrNull(name) != nullptr;
  };
  std::set<absl::string_view> missing_operators;
  for (absl::string_view operator_name :
       operator_package_proto.required_registered_operators()) {
    if (!check_registered_operator_presence(operator_name)) {
      missing_operators.insert(operator_name);
    }
  }
  if (!missing_operators.empty()) {
    return absl::FailedPreconditionError(
        "missing dependencies: M." + absl::StrJoin(missing_operators, ", M."));
  }

  // Check operators that already registered.
  std::set<absl::string_view> already_registered_operators;
  for (const auto& operator_proto : operator_package_proto.operators()) {
    if (check_registered_operator_presence(
            operator_proto.registration_name())) {
      already_registered_operators.insert(operator_proto.registration_name());
    }
  }

  if (!already_registered_operators.empty()) {
    return absl::FailedPreconditionError(
        "already present in the registry: M." +
        absl::StrJoin(already_registered_operators, ", M."));
  }

  // Load operators.
  for (int i = 0; i < operator_package_proto.operators_size(); ++i) {
    const auto& operator_proto = operator_package_proto.operators(i);
    ASSIGN_OR_RETURN(auto decode_result,
                     serialization::Decode(operator_proto.implementation()),
                     _ << "operators[" << i << "].registration_name="
                       << operator_proto.registration_name());
    if (decode_result.values.size() != 1 || !decode_result.exprs.empty()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "expected to get a value, got %d values and %d exprs; "
          "operators[%d].registration_name=%s",
          decode_result.values.size(), decode_result.exprs.size(), i,
          operator_proto.registration_name()));
    }
    const auto& qvalue = decode_result.values[0];
    if (qvalue.GetType() != GetQType<ExprOperatorPtr>()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "expected to get %s, got %s; operators[%d].registration_name=%s",
          GetQType<ExprOperatorPtr>()->name(), qvalue.GetType()->name(), i,
          operator_proto.registration_name()));
    }
    RETURN_IF_ERROR(operator_registry
                        ->Register(operator_proto.registration_name(),
                                   qvalue.UnsafeAs<ExprOperatorPtr>())
                        .status());
  }
  return absl::OkStatus();
}

}  // namespace arolla::operator_package
