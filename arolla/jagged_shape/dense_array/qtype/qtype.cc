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
#include "arolla/jagged_shape/dense_array/qtype/qtype.h"

#include "arolla/dense_array/edge.h"
#include "arolla/dense_array/qtype/types.h"
#include "arolla/jagged_shape/dense_array/jagged_shape.h"
#include "arolla/jagged_shape/qtype/qtype.h"
#include "arolla/qtype/qtype.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/util/fingerprint.h"
#include "arolla/util/indestructible.h"
#include "arolla/util/init_arolla.h"
#include "arolla/util/meta.h"
#include "arolla/util/repr.h"

namespace arolla {
namespace {

// Not movable or copyable.
class JaggedDenseArrayShapeQType final : public JaggedShapeQType {
 public:
  static const JaggedDenseArrayShapeQType* GetInstance() {
    static Indestructible<JaggedDenseArrayShapeQType> result;
    return result.get();
  }

  JaggedDenseArrayShapeQType()
      : JaggedShapeQType(meta::type<JaggedDenseArrayShapePtr>(),
                         "JAGGED_DENSE_ARRAY_SHAPE") {}

  QTypePtr edge_qtype() const override { return GetQType<DenseArrayEdge>(); };
};

}  // namespace


QTypePtr QTypeTraits<JaggedDenseArrayShapePtr>::type() {
  return JaggedDenseArrayShapeQType::GetInstance();
}

void FingerprintHasherTraits<JaggedDenseArrayShapePtr>::operator()(
    FingerprintHasher* hasher, const JaggedDenseArrayShapePtr& value) const {
  hasher->Combine(*value);
}

ReprToken ReprTraits<JaggedDenseArrayShapePtr>::operator()(
    const JaggedDenseArrayShapePtr& value) const {
  return GenReprToken(*value);
}

AROLLA_REGISTER_ANONYMOUS_INITIALIZER(kHighest, [] {
  return SetEdgeQTypeToJaggedShapeQType(GetQType<DenseArrayEdge>(),
                                        GetQType<JaggedDenseArrayShapePtr>());
})

}  // namespace arolla
