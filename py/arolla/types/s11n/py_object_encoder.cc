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
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "py/arolla/types/qtype/py_object_boxing.h"
#include "py/arolla/types/qvalue/py_function_operator.h"
#include "py/arolla/types/s11n/codec_name.h"
#include "py/arolla/types/s11n/py_object_codec.pb.h"
#include "arolla/expr/expr_operator.h"
#include "arolla/expr/expr_operator_signature.h"
#include "arolla/qtype/qtype.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/qtype/typed_ref.h"
#include "arolla/serialization/encode.h"
#include "arolla/serialization_base/encode.h"
#include "arolla/util/init_arolla.h"
#include "arolla/util/status_macros_backport.h"

namespace arolla::python {
namespace {

using ::arolla::expr::ExprOperatorPtr;
using ::arolla::serialization::RegisterValueEncoderByQType;
using ::arolla::serialization::RegisterValueEncoderByQValueSpecialisationKey;
using ::arolla::serialization_base::Encoder;
using ::arolla::serialization_base::ValueProto;

ValueProto GenValueProto(Encoder& encoder) {
  ValueProto value_proto;
  value_proto.set_codec_index(encoder.EncodeCodec(kPyObjectV1Codec));
  return value_proto;
}

absl::StatusOr<ValueProto> EncodePyObjectQValue(TypedRef value,
                                                Encoder& encoder) {
  if (value.GetType() == GetQType<QTypePtr>()) {
    auto value_proto = GenValueProto(encoder);
    value_proto.MutableExtension(PyObjectV1Proto::extension)
        ->set_py_object_qtype(true);
    return value_proto;
  } else if (value.GetType() == GetPyObjectQType()) {
    ASSIGN_OR_RETURN(auto maybe_codec, GetPyObjectCodec(value));
    if (!maybe_codec) {
      return absl::InvalidArgumentError(
          absl::StrFormat("missing serialization codec for %s", value.Repr()));
    }
    ASSIGN_OR_RETURN(auto data, EncodePyObject(value));
    auto value_proto = GenValueProto(encoder);
    auto* py_obj_proto =
        value_proto.MutableExtension(PyObjectV1Proto::extension);
    py_obj_proto->mutable_py_object_value()->set_codec(*maybe_codec);
    py_obj_proto->mutable_py_object_value()->set_data(data);
    return value_proto;
  } else {
    return absl::UnimplementedError(absl::StrFormat(
        "%s does not support serialization of %s: %s", kPyObjectV1Codec,
        value.GetType()->name(), value.Repr()));
  }
}

absl::StatusOr<ValueProto> EncodePyFunctionOperator(TypedRef value,
                                                    Encoder& encoder) {
  if (value.GetType() != GetQType<ExprOperatorPtr>()) {
    return absl::UnimplementedError(absl::StrFormat(
        "%s does not support serialization of %s: %s", kPyObjectV1Codec,
        value.GetType()->name(), value.Repr()));
  }
  const auto& op_value = *value.UnsafeAs<ExprOperatorPtr>();
  if (typeid(op_value) != typeid(PyFunctionOperator)) {
    return absl::UnimplementedError(absl::StrFormat(
        "%s does not support serialization of %s: %s", kPyObjectV1Codec,
        value.GetType()->name(), value.Repr()));
  }
  const auto& op = static_cast<const PyFunctionOperator&>(op_value);
  auto value_proto = GenValueProto(encoder);
  auto* op_proto = value_proto.MutableExtension(PyObjectV1Proto::extension)
                       ->mutable_py_function_operator_value();
  const auto& name = op.display_name();
  op_proto->set_name(name.data(), name.size());
  op_proto->set_signature_spec(GetExprOperatorSignatureSpec(op.signature()));
  op_proto->set_doc(op.doc());
  // Encode and set the objects.
  ASSIGN_OR_RETURN(auto encoded_qtype_inference_expr,
                   encoder.EncodeExpr(op.GetQTypeInferenceExpr()),
                   _ << "GetQTypeInferenceExpr(); value=PY_FUNCTION_OPERATOR "
                        "with name="
                     << op.display_name());
  ASSIGN_OR_RETURN(
      auto encoded_eval_fn, encoder.EncodeValue(op.GetPyEvalFn()),
      _ << "py_obj=PyEvalFn(); value=PY_FUNCTION_OPERATOR with name="
        << op.display_name());
  value_proto.add_input_expr_indices(encoded_qtype_inference_expr);
  value_proto.add_input_value_indices(encoded_eval_fn);
  // Sets default values for the operator signature.
  for (const auto& param : op.signature().parameters) {
    if (param.default_value.has_value()) {
      ASSIGN_OR_RETURN(auto value_index,
                       encoder.EncodeValue(param.default_value.value()));
      value_proto.add_input_value_indices(value_index);
    }
  }
  return value_proto;
}

AROLLA_REGISTER_INITIALIZER(
    kRegisterSerializationCodecs,
    register_serialization_codecs_py_object_v1_encoder, []() -> absl::Status {
      RETURN_IF_ERROR(RegisterValueEncoderByQValueSpecialisationKey(
          "::arolla::python::PyFunctionOperator", EncodePyFunctionOperator));
      RETURN_IF_ERROR(RegisterValueEncoderByQType(GetPyObjectQType(),
                                                  EncodePyObjectQValue));
      return absl::OkStatus();
    })

}  // namespace

}  // namespace arolla::python
