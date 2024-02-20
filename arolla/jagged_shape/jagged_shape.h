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
#ifndef AROLLA_JAGGED_SHAPE_JAGGED_SHAPE_H_
#define AROLLA_JAGGED_SHAPE_JAGGED_SHAPE_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "arolla/memory/raw_buffer_factory.h"
#include "arolla/util/fingerprint.h"
#include "arolla/util/status_macros_backport.h"

namespace arolla {

template <typename Edge>
class JaggedShape;

// Shared pointer for JaggedShape.
//
// NOTE: Nonnull by convention. Use absl::Nullable to explicitly mark nullable
// pointers.
template <typename Edge>
using JaggedShapePtr = std::shared_ptr<JaggedShape<Edge>>;

// Shape that represents multidimensional jagged data. Each dimension `i` is
// represented using an array-to-array Edge with one edge per dimension
// (edges().size() == rank()). edges[i + 1] specifies how to partition the rows
// in the i'th dimension, partitioning edges[i + 1].parent_size() rows in the
// i'th dimension into edges[i + 1].child_size() rows in the (i+1)th dimension.
//
// The shape is represented (printed) using _sizes_, where uniform edges (where
// all splits are the same size) are represented by a single value.
//
// Requirements for each edge:
//   - edges[0].parent_size() == 1
//   - edges[i + 1].parent_size() == edges[i].child_size(), for all i.
//   - edges[i] must be representable using split points.
//     - mapping edges will be converted to split point edges.
//
// See go/jagged-shape for details.
template <typename EdgeT>
class JaggedShape {
  struct PrivateConstructorTag {};

 public:
  // Note: using absl::InlinedVector<Edge, 4> is generally slower for our uses.
  // This includes construction of empty shapes since shared_ptr is used. If
  // e.g. JaggedShape::Empty is changed to not return a shared_ptr,
  // absl::InlinedVector will likely be faster again.
  using EdgeVec = std::vector<EdgeT>;
  using Edge = EdgeT;
  using ShapePtr = JaggedShapePtr<EdgeT>;

  // Creates an empty shape (rank 0, size 1).
  static ShapePtr Empty() {
    return std::make_shared<JaggedShape>(PrivateConstructorTag{});
  }

  // Creates a JaggedShape from edges, and ensures that the resulting shape is
  // sound. Requirements:
  //   - edges[0].parent_size() == 1
  //   - edges[i + 1].parent_size() == edges[i].child_size(), for all i.
  //   - edges[i] must be representable using split points.
  //     - mapping edges will be converted to split point edges.
  //
  // `buf_factory` specifies the memory location of the converted split points.
  static absl::StatusOr<ShapePtr> FromEdges(
      EdgeVec edges, RawBufferFactory& buf_factory = *GetHeapBufferFactory()) {
    if (edges.empty()) return Empty();
    int64_t child_size = 1;
    for (size_t i = 0; i < edges.size(); ++i) {
      if (edges[i].parent_size() != child_size) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "incompatible edges - edges[%d].parent_size != %d (prior edge's "
            "child_size)",
            i, child_size));
      }
      if (edges[i].edge_type() != Edge::SPLIT_POINTS) {
        ASSIGN_OR_RETURN(edges[i], edges[i].ToSplitPointsEdge(buf_factory));
      }
      child_size = edges[i].child_size();
    }
    return std::make_shared<JaggedShape>(PrivateConstructorTag{},
                                         std::move(edges));
  }

  // Creates a 1-dimensional JaggedShape from the size. This is especially
  // useful when creating a JaggedShape representing Array / DenseArray values.
  static ShapePtr FlatFromSize(
      int64_t size, RawBufferFactory& buf_factory = *GetHeapBufferFactory()) {
    auto edge_or = Edge::FromUniformGroups(/*parent_size=*/1,
                                           /*group_size=*/size, buf_factory);
    DCHECK_OK(edge_or.status());  // Cannot fail for valid shapes.
    auto shape_or = FromEdges({std::move(*edge_or)});
    DCHECK_OK(shape_or.status());  // Cannot fail for valid shapes.
    return std::move(*shape_or);
  }

  // Returns the rank of the shape.
  size_t rank() const { return edges_.size(); }

  // Returns the size of the shape, which equals the total number of
  // corresponding elements.
  //   * rank() == 0 -> scalar -> size() == 1.
  //   * rank() > 0 -> non-scalar -> size() == edges().back().child_size().
  int64_t size() const {
    return edges_.empty() ? 1 : edges_.back().child_size();
  }

  // Returns the edges of the shape. The size of the span is always equal to the
  // rank.
  absl::Span<const Edge> edges() const { return edges_; }

  // Returns a copy of this shape with `edges` appended. Has the same
  // restrictions as JaggedShape::FromEdges.
  //
  // `buf_factory` specifies the memory location of the converted split points.
  absl::StatusOr<ShapePtr> AddDims(
      absl::Span<const Edge> edges,
      RawBufferFactory& buf_factory = *GetHeapBufferFactory()) const {
    EdgeVec new_edges = edges_;
    new_edges.insert(new_edges.end(), edges.begin(), edges.end());
    return JaggedShape::FromEdges(std::move(new_edges), buf_factory);
  }

  // Returns a copy of this shape containing the dims [0, from). Requires
  // 0 <= from <= rank().
  ShapePtr RemoveDims(size_t from) const {
    DCHECK_GE(from, 0);
    DCHECK_LE(from, rank());
    EdgeVec new_edges{edges_.begin(), edges_.begin() + from};
    return std::make_shared<JaggedShape>(PrivateConstructorTag{},
                                         std::move(new_edges));
  }

  // Flattens the dimensions between [from, to) into a single dimension,
  // or inserts a "unit" dimension at `from` when `from` == `to`.
  //
  // Requires `0 <= from <= to <= rank()`.
  // The resulting shape has `rank() == old_rank - (to - from) + 1`.
  //
  // Example:
  //   shape = JaggedShape([[0, 2], [0, 1, 3], [0, 1, 2, 4]])
  //   shape.FlattenDims(1, 3) -> JaggedShape([[0, 2], [0, 1, 4]]).
  //
  // Unit dimension example:
  //   shape = JaggedShape([[0, 2], [0, 1, 3]])
  //   shape.FlattenDims(1, 1) -> JaggedShape([[0, 2], [0, 1, 2], [0, 1, 3]]).
  ShapePtr FlattenDims(
      size_t from, size_t to,
      RawBufferFactory& buf_factory = *GetHeapBufferFactory()) const {
    DCHECK_GE(from, 0);
    DCHECK_LE(to, rank());
    DCHECK_LE(from, to);
    if (to - from == 1) return std::make_shared<JaggedShape>(*this);
    if (to - from == rank()) return FlatFromSize(size(), buf_factory);
    EdgeVec new_edges;
    new_edges.reserve(rank() - (to - from) + 1);
    new_edges.insert(new_edges.end(), edges_.begin(), edges_.begin() + from);
    if (from == to) {
      // Insert a unit-edge at `from`.
      int64_t parent_size = from == 0 ? 1 : edges_[from - 1].child_size();
      auto unit_edge =
          Edge::FromUniformGroups(parent_size, /*group_size=*/1, buf_factory);
      DCHECK_OK(unit_edge.status());  // Cannot fail for valid shapes.
      new_edges.push_back(std::move(*unit_edge));
    } else {
      auto composed_edge = Edge::ComposeEdges(
          absl::MakeConstSpan(edges_).subspan(from, to - from), buf_factory);
      DCHECK_OK(composed_edge.status());  // Cannot fail for valid shapes.
      new_edges.push_back(std::move(*composed_edge));
    }
    new_edges.insert(new_edges.end(), edges_.begin() + to, edges_.end());
    return std::make_shared<JaggedShape>(PrivateConstructorTag{},
                                         std::move(new_edges));
  }

  // Heuristically checks if this_shape == that_shape. Note:
  //   - May return false positives, but not false negatives.
  //   - Commutative.
  bool IsProbablyEquivalentTo(const JaggedShape& other) const {
    if (rank() != other.rank()) return false;
    for (size_t i = 0; i < rank(); ++i) {
      const auto& this_edge = edges_[i];
      const auto& that_edge = other.edges_[i];
      if (this_edge.parent_size() != that_edge.parent_size() ||
          this_edge.child_size() != that_edge.child_size()) {
        return false;
      }
    }
    return true;
  }

  // Checks if this_shape == that_shape.
  bool IsEquivalentTo(const JaggedShape& other) const {
    if (!IsProbablyEquivalentTo(other)) return false;
    for (size_t i = 0; i < rank(); ++i) {
      if (!edges_[i].IsEquivalentTo(other.edges_[i])) return false;
    }
    return true;
  }

  // Returns true if `this` is a prefix of `other`. This means that `other`
  // shape edges in the front are all equivalent and in the same order as edges
  // in `this`.
  //
  // Equivalent shapes are also expandable to each other.
  bool IsBroadcastableTo(const JaggedShape& other) const {
    if (other.rank() < rank()) return false;
    for (int i = 0; i < rank(); ++i) {
      if (!edges_[i].IsEquivalentTo(other.edges_[i])) return false;
    }
    return true;
  }

  // Returns an Edge that broadcasts `this` to `other`, such that
  // `size() == other.size()`.
  //
  // Requires `shape.IsBroadcastableTo(other)`.
  //
  // Example:
  //   flat_values = [1, 2, 3]
  //   this_shape = [[0, 2], [0, 2, 3]]
  //   other_shape = [[0, 2], [0, 2, 3], [0, 1, 3, 4]]
  //   edge = this_shape.GetBroadcastEdge(other_shape)
  //     # Returns: [0, 1, 3, 4]
  //   flat_values.Expand(edge)
  //     # Returns: [1, 2, 2, 3]
  Edge GetBroadcastEdge(
      const JaggedShape& other,
      RawBufferFactory& buf_factory = *GetHeapBufferFactory()) const {
    DCHECK(IsBroadcastableTo(other));
    if (rank() == other.rank()) {
      auto unit_edge =
          Edge::FromUniformGroups(size(), /*group_size=*/1, buf_factory);
      DCHECK_OK(unit_edge.status());
      return std::move(*unit_edge);
    } else {
      auto res_edge = Edge::ComposeEdges(
          absl::MakeConstSpan(other.edges_).subspan(rank()), buf_factory);
      DCHECK_OK(res_edge.status());
      return std::move(*res_edge);
    }
  }

  // Creates an empty shape (rank 0, size 0).
  explicit JaggedShape(PrivateConstructorTag) {}

  JaggedShape(PrivateConstructorTag, EdgeVec edges)
      : edges_(std::move(edges)) {}

 private:
  EdgeVec edges_;
};

template <typename Edge>
struct FingerprintHasherTraits<JaggedShape<Edge>> {
  void operator()(FingerprintHasher* hasher,
                  const JaggedShape<Edge>& value) const {
    hasher->Combine(value.rank());
    for (const auto& edge : value.edges()) {
      hasher->Combine(edge);
    }
  }
};

}  // namespace arolla

#endif  // AROLLA_JAGGED_SHAPE_JAGGED_SHAPE_H_
