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
#include "arolla/qexpr/operators.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "arolla/memory/frame.h"
#include "arolla/qexpr/casting.h"
#include "arolla/qexpr/eval_context.h"
#include "arolla/qexpr/operator_errors.h"
#include "arolla/qexpr/qexpr_operator_signature.h"
#include "arolla/qtype/qtype.h"
#include "arolla/qtype/typed_slot.h"
#include "arolla/qtype/typed_value.h"
#include "arolla/util/indestructible.h"
#include "arolla/util/operator_name.h"
#include "arolla/util/status_macros_backport.h"

namespace arolla {
namespace {

// QExprOperator family that stores several independent operators sharing the
// same nspace::name.
class CombinedOperatorFamily : public OperatorFamily {
 public:
  explicit CombinedOperatorFamily(std::string name) : name_(std::move(name)) {}

  absl::StatusOr<OperatorPtr> DoGetOperator(
      absl::Span<const QTypePtr> input_types,
      QTypePtr output_type) const override {
    // TODO: try to avoid temporary vector.
    std::vector<QTypePtr> input_types_arg(input_types.begin(),
                                          input_types.end());
    auto op = operators_.find(input_types_arg);
    if (op != operators_.end() &&
        op->second->GetQType()->GetOutputType() == output_type) {
      return op->second;
    }

    ASSIGN_OR_RETURN(const QExprOperatorSignature* matching_qtype,
                     FindMatchingSignature(
                         QExprOperatorSignature::Get(input_types, output_type),
                         supported_qtypes_, name_));

    return operators_.at(
        std::vector<QTypePtr>(matching_qtype->GetInputTypes().begin(),
                              matching_qtype->GetInputTypes().end()));
  }

  // Tries to insert an operator. Returns an error if an operator with the
  // same input types is already registered.
  absl::Status Insert(OperatorPtr op) {
    auto input_types = op->GetQType()->GetInputTypes();
    std::vector<QTypePtr> input_types_arg(input_types.begin(),
                                          input_types.end());
    auto inserted = operators_.emplace(input_types_arg, std::move(op));
    if (!inserted.second) {
      // TODO (b/281584281): Generate warning only in unexpected cases.
      // return absl::Status(
      //     absl::StatusCode::kAlreadyExists,
      //     absl::StrFormat("trying to register QExpr operator %s%s twice",
      //     name_,
      //     FormatTypeVector(input_types)));
      return absl::OkStatus();
    }
    supported_qtypes_.push_back(inserted.first->second->GetQType());
    return absl::OkStatus();
  }

 private:
  std::string name_;
  absl::flat_hash_map<std::vector<QTypePtr>, OperatorPtr> operators_;
  std::vector<const QExprOperatorSignature*> supported_qtypes_;
};

}  // namespace

absl::Status OperatorRegistry::RegisterOperatorFamily(
    absl::string_view name, std::unique_ptr<OperatorFamily> operation) {
  absl::WriterMutexLock lock(&mutex_);

  if (!IsOperatorName(name)) {
    return absl::InvalidArgumentError(
        absl::StrFormat("incorrect operator name \"%s\"", name));
  }

  auto inserted = families_.emplace(name, std::move(operation));
  if (!inserted.second) {
    return absl::Status(
        absl::StatusCode::kAlreadyExists,
        absl::StrFormat(
            "trying to register non-static QExpr operator family %s twice",
            name));
  }
  return absl::OkStatus();
}

absl::Status OperatorRegistry::RegisterOperator(OperatorPtr op) {
  absl::WriterMutexLock lock(&mutex_);

  if (!IsOperatorName(op->name())) {
    return absl::InvalidArgumentError(
        absl::StrFormat("incorrect operator name \"%s\"", op->name()));
  }

  auto found = families_.find(op->name());
  if (found == families_.end()) {
    auto inserted = families_.emplace(
        op->name(),
        std::make_unique<CombinedOperatorFamily>(std::string(op->name())));
    found = inserted.first;
  }

  if (auto* combined_family =
          dynamic_cast<CombinedOperatorFamily*>(found->second.get())) {
    return combined_family->Insert(std::move(op));
  } else {
    return absl::Status(
        absl::StatusCode::kAlreadyExists,
        absl::StrFormat("trying to register a single QExpr operator and an "
                        "operator family under the same name %s",
                        op->name()));
  }
}

std::vector<std::string> OperatorRegistry::ListRegisteredOperators() {
  absl::ReaderMutexLock lock(&mutex_);

  std::vector<std::string> names;
  names.reserve(families_.size());
  for (const auto& [name, family] : families_) {
    names.push_back(name);
  }
  return names;
}

absl::StatusOr<const OperatorFamily*> OperatorRegistry::LookupOperatorFamily(
    absl::string_view name) const {
  absl::ReaderMutexLock lock(&mutex_);

  auto iter = families_.find(name);
  if (iter == families_.end()) {
    return absl::Status(absl::StatusCode::kNotFound,
                        absl::StrFormat("QExpr operator %s not found; %s", name,
                                        SuggestMissingDependency()));
  }
  return iter->second.get();
}

absl::StatusOr<OperatorPtr> OperatorRegistry::DoLookupOperator(
    absl::string_view name, absl::Span<const QTypePtr> input_types,
    QTypePtr output_type) const {
  ASSIGN_OR_RETURN(auto family, LookupOperatorFamily(name));
  return family->GetOperator(input_types, output_type);
}

OperatorRegistry* OperatorRegistry::GetInstance() {
  static Indestructible<OperatorRegistry> instance(
      [](auto* self) { new (self) OperatorRegistry; });
  return instance.get();
}

namespace {

// A bound operator with a minimal frame layout needed to bind it.
struct BoundOperatorState {
  std::unique_ptr<BoundOperator> op;
  std::vector<TypedSlot> input_slots;
  TypedSlot output_slot;
  FrameLayout layout;
};

// Creates a minimal frame layout required to execute the operator and binds
// the operator to it.
absl::StatusOr<BoundOperatorState> BindToNewLayout(const QExprOperator& op) {
  FrameLayout::Builder layout_builder;
  auto input_slots = AddSlots(op.GetQType()->GetInputTypes(), &layout_builder);
  auto output_slot = AddSlot(op.GetQType()->GetOutputType(), &layout_builder);
  ASSIGN_OR_RETURN(auto bound_op, op.Bind(input_slots, output_slot));
  return BoundOperatorState{std::move(bound_op), input_slots, output_slot,
                            std::move(layout_builder).Build()};
}

// Verifies operator's input and output slot types to match
// QExprOperatorSignature.
absl::Status VerifyOperatorSlots(const QExprOperator& op,
                                 absl::Span<const TypedSlot> input_slots,
                                 TypedSlot output_slot) {
  auto qtype = op.GetQType();
  RETURN_IF_ERROR(
      VerifyInputSlotTypes(input_slots, qtype->GetInputTypes(), op.name()));
  return VerifyOutputSlotType(output_slot, qtype->GetOutputType(), op.name());
}

}  // namespace

absl::StatusOr<OperatorPtr> EnsureOutputQTypeMatches(
    absl::StatusOr<OperatorPtr> op_or, absl::Span<const QTypePtr> input_types,
    QTypePtr output_type) {
  ASSIGN_OR_RETURN(auto op, op_or);
  if (op->GetQType()->GetOutputType() != output_type) {
    return absl::Status(
        absl::StatusCode::kNotFound,
        absl::StrFormat(
            "operator %s%s->%s not found: unexpected output type %s",
            op->name(), FormatTypeVector(input_types), output_type->name(),
            op->GetQType()->GetOutputType()->name()));
  }
  return op;
}

absl::StatusOr<TypedValue> InvokeOperator(const QExprOperator& op,
                                          absl::Span<const TypedValue> args) {
  RETURN_IF_ERROR(
      VerifyInputValueTypes(args, op.GetQType()->GetInputTypes(), op.name()));
  ASSIGN_OR_RETURN(auto bound, BindToNewLayout(op));
  RootEvaluationContext root_ctx(&bound.layout);

  // Copy inputs to the temporary context.
  for (size_t i = 0; i < args.size(); ++i) {
    RETURN_IF_ERROR(args[i].CopyToSlot(bound.input_slots[i], root_ctx.frame()));
  }
  EvaluationContext ctx(root_ctx);
  bound.op->Run(&ctx, root_ctx.frame());
  if (!ctx.status().ok()) {
    return std::move(ctx).status();
  }
  return TypedValue::FromSlot(bound.output_slot, root_ctx.frame());
}

// Returns the result of an operator evaluation with given inputs.
absl::StatusOr<TypedValue> InvokeOperator(absl::string_view op_name,
                                          absl::Span<const TypedValue> args,
                                          QTypePtr output_qtype) {
  std::vector<QTypePtr> arg_types;
  arg_types.reserve(args.size());
  for (const auto& arg : args) {
    arg_types.push_back(arg.GetType());
  }
  ASSIGN_OR_RETURN(auto op, OperatorRegistry::GetInstance()->LookupOperator(
                                op_name, arg_types, output_qtype));
  return InvokeOperator(*op, args);
}

absl::StatusOr<std::unique_ptr<BoundOperator>> QExprOperator::Bind(
    absl::Span<const TypedSlot> input_slots, TypedSlot output_slot) const {
  RETURN_IF_ERROR(VerifyOperatorSlots(*this, input_slots, output_slot));
  return DoBind(input_slots, output_slot);
}

}  // namespace arolla
