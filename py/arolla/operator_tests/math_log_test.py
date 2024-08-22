# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Tests for M.math.log operator."""

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
from arolla.operator_tests import pointwise_test_utils
import numpy as np

NAN = float("nan")
INF = float("inf")

M = arolla.M


def gen_test_data():
  """Returns scalar test data for math.log operator.

  Returns: tuple(
      (arg, result),
      ...
  )
  """
  values = (1, 10.5, 100, 1000, INF, NAN)
  return (
      (None, None),
      (-INF, NAN),
      (-1, NAN),
      (0, -INF),
  ) + tuple((arg, float(np.log(arg))) for arg in values)


TEST_DATA = gen_test_data()

# QType signatures: tuple(
#     (arg_qtype, result_qtype),
#     ...
# )
QTYPE_SIGNATURES = tuple(
    (arg, arolla.types.common_float_qtype(arg))
    for arg in pointwise_test_utils.lift_qtypes(*arolla.types.NUMERIC_QTYPES)
)


class MathLogTest(parameterized.TestCase):

  def testQTypeSignatures(self):
    arolla.testing.assert_qtype_signatures(M.math.log, QTYPE_SIGNATURES)

  @parameterized.parameters(
      pointwise_test_utils.gen_cases(TEST_DATA, *QTYPE_SIGNATURES)
  )
  def testValue(self, arg, expected_value):
    actual_value = arolla.eval(M.math.log(arg))
    arolla.testing.assert_qvalue_allclose(actual_value, expected_value)


if __name__ == "__main__":
  absltest.main()