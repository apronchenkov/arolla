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

"""Tests for core.any."""

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
from arolla.operator_tests import backend_test_base
from arolla.operator_tests import pointwise_test_utils
from arolla.operator_tests import utils

M = arolla.M


def agg_into_scalar(x):
  if x.value_qtype != arolla.UNIT:
    return utils.skip_case
  return arolla.optional_unit(x.present_count > 0 or None)


def _gen_scalar_cases():
  values = (
      (arolla.unit(), arolla.unit()),
      (arolla.optional_unit(True), arolla.optional_unit(True)),
      (arolla.missing_unit(), arolla.optional_unit(None)),
  )
  for (arg, res) in values:
    yield (arg, res)
    yield (arg, arolla.unspecified(), res)
    yield (arg, arolla.types.ScalarToScalarEdge(), res)

TEST_CASES = tuple(utils.gen_simple_agg_into_cases(agg_into_scalar)) + tuple(
    _gen_scalar_cases()
)

QTYPE_SIGNATURES = frozenset(
    tuple(x.qtype for x in test_case) for test_case in TEST_CASES
)


class CoreAnyTest(parameterized.TestCase, backend_test_base.SelfEvalMixin):

  def test_qtype_signatures(self):
    self.require_self_eval_is_called = False
    self.assertCountEqual(
        pointwise_test_utils.detect_qtype_signatures(M.core.any),
        QTYPE_SIGNATURES,
    )

  @parameterized.parameters(*TEST_CASES)
  def test_eval(self, *test_case):
    args = test_case[:-1]
    expected_result = test_case[-1]
    arolla.testing.assert_qvalue_allequal(
        self.eval(M.core.any(*args)), expected_result
    )


if __name__ == "__main__":
  absltest.main()
