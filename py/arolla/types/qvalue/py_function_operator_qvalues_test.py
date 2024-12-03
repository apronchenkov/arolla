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

import gc
import inspect
import re
import sys
import traceback

from absl.testing import absltest
from absl.testing import parameterized
from arolla.abc import abc as arolla_abc
from arolla.operators import operators_clib as _
from arolla.testing import testing as arolla_testing
from arolla.types.qtype import array_qtypes
from arolla.types.qtype import boxing
from arolla.types.qtype import casting
from arolla.types.qtype import scalar_qtypes
from arolla.types.qtype import tuple_qtypes
from arolla.types.qvalue import clib
from arolla.types.qvalue import lambda_operator_qvalues
from arolla.types.qvalue import py_function_operator_qvalues
from arolla.types.qvalue import scalar_qvalues as _

l_x = arolla_abc.leaf('x')
l_y = arolla_abc.leaf('y')
p_x = arolla_abc.placeholder('x')
p_y = arolla_abc.placeholder('y')
p_z = arolla_abc.placeholder('z')
p_w = arolla_abc.placeholder('w')
p_v = arolla_abc.placeholder('v')
p_args = arolla_abc.placeholder('args')

common_qtype_op = arolla_abc.lookup_operator('qtype.common_qtype')
make_tuple_qtype_op = arolla_abc.lookup_operator('qtype.make_tuple_qtype')
floordiv_op = arolla_abc.lookup_operator('math.floordiv')
is_numeric_qtype = arolla_abc.lookup_operator('qtype.is_numeric_qtype')


def add(x, y):
  return x + y


ADD_QTYPE = common_qtype_op(p_x, p_y)


def gen_eval_testcases():
  yield (
      lambda x, y: x + y,
      (scalar_qtypes.int32(1), scalar_qtypes.int32(2)),
      scalar_qtypes.int32(3),
  )
  yield (
      lambda *args: sum(args, 0.0),
      (
          scalar_qtypes.float32(1),
          scalar_qtypes.float32(2),
          scalar_qtypes.float64(3),
      ),
      scalar_qtypes.float64(6),
  )
  yield (
      lambda x, y=1: x + y,
      (array_qtypes.array_int32([1, 2]),),
      array_qtypes.array_int32([2, 3]),
  )

  # Testing defaults + varargs combination.
  fn = lambda x, y=1, *args: x + y + sum(args)
  yield (
      fn,
      (array_qtypes.array_int32([1, 2]),),
      array_qtypes.array_int32([2, 3]),
  )
  yield (
      fn,
      (array_qtypes.array_int32([1, 2]), scalar_qtypes.int32(2)),
      array_qtypes.array_int32([3, 4]),
  )
  yield (
      fn,
      (
          array_qtypes.array_int32([1, 2]),
          scalar_qtypes.int32(2),
          scalar_qtypes.int32(3),
      ),
      array_qtypes.array_int32([6, 7]),
  )

  # Function with context.
  y = 1
  fn = lambda x: x + y
  yield (
      fn,
      (array_qtypes.array_int32([1, 2]),),
      array_qtypes.array_int32([2, 3]),
  )

  # Callable.
  class CallableClass:

    def __init__(self, y):
      self.y = y

    def __call__(self, x):
      return x + self.y

  yield (
      CallableClass(1),
      (array_qtypes.array_int32([1, 2]),),
      array_qtypes.array_int32([2, 3]),
  )


EVAL_TEST_CASES = tuple(gen_eval_testcases())


class PyFunctionOperatorQValueTest(parameterized.TestCase):

  def setUp(self):
    super().setUp()
    self.add_op = py_function_operator_qvalues.PyFunctionOperator(
        'test.add',
        'x, y',
        arolla_abc.PyObject(add),
        qtype_inference_expr=ADD_QTYPE,
        doc='add docstring',
    )

  def test_name(self):
    self.assertEqual(self.add_op.display_name, 'test.add')

  def test_docstring(self):
    self.assertIn('PyFunctionOperator', type(self.add_op).__doc__)
    self.assertEqual(self.add_op.getdoc(), 'add docstring')

  def test_signature(self):
    op = py_function_operator_qvalues.PyFunctionOperator(
        'test.foo',
        'x, y',
        arolla_abc.PyObject(lambda a, b: None),
        qtype_inference_expr=p_x,
    )
    self.assertEqual(
        inspect.signature(op), inspect.signature(lambda x, y: None)
    )

  def test_fingerprinting(self):
    # Fingerprinting is random.
    add_fn = arolla_abc.PyObject(add)
    op1 = py_function_operator_qvalues.PyFunctionOperator(
        'test.add', 'x, y', add_fn, qtype_inference_expr=ADD_QTYPE
    )
    op2 = py_function_operator_qvalues.PyFunctionOperator(
        'test.add', 'x, y', add_fn, qtype_inference_expr=ADD_QTYPE
    )
    self.assertNotEqual(op1.fingerprint, op2.fingerprint)

  def test_eval_fn_not_callable(self):
    with self.assertRaisesRegex(
        TypeError,
        r'expected `eval_fn\.py_value\(\)` to be a callable, was.*int',
    ):
      py_function_operator_qvalues.PyFunctionOperator(
          'test.add',
          'x, y',
          arolla_abc.PyObject(1),
          qtype_inference_expr=ADD_QTYPE,
      )

  def test_qtype_inference_expr_not_expr(self):
    with self.assertRaises(TypeError):
      py_function_operator_qvalues.PyFunctionOperator(
          'test.add',
          'x, y',
          arolla_abc.PyObject(add),
          qtype_inference_expr=lambda x: x,  # pytype: disable=wrong-arg-types
      )

  def test_qtype_inference_expr_not_qtype_output(self):
    with self.assertRaisesRegex(
        ValueError,
        re.escape(
            'expected a qtype inference expression to return QTYPE, got INT32'
        ),
    ):
      py_function_operator_qvalues.PyFunctionOperator(
          'test.add',
          'x, y',
          arolla_abc.PyObject(add),
          qtype_inference_expr=boxing.as_expr(1),
      )

  def test_qtype_inference_expr_not_matching(self):
    with self.assertRaisesRegex(ValueError, 'unexpected parameters: P.w, P.z'):
      py_function_operator_qvalues.PyFunctionOperator(
          'test.add',
          'x, y',
          arolla_abc.PyObject(add),
          qtype_inference_expr=common_qtype_op(common_qtype_op(p_x, p_z), p_w),
      )

  def test_qtype_constraints_not_matching(self):
    with self.assertRaisesRegex(ValueError, 'unexpected parameters: P.w, P.z'):
      py_function_operator_qvalues.PyFunctionOperator(
          'test.add',
          'x, y',
          arolla_abc.PyObject(add),
          qtype_constraints=[
              (is_numeric_qtype(p_x), ''),
              (is_numeric_qtype(p_y), ''),
              (
                  is_numeric_qtype(
                      common_qtype_op(p_x, common_qtype_op(p_w, p_z))
                  ),
                  '',
              ),
              (is_numeric_qtype(p_v), ''),
          ],
          qtype_inference_expr=ADD_QTYPE,
      )


class PyFunctionOperatorQValueEvalTest(parameterized.TestCase):

  def setUp(self):
    super().setUp()
    self.add_op = py_function_operator_qvalues.PyFunctionOperator(
        'test.add',
        'x, y',
        arolla_abc.PyObject(add),
        qtype_inference_expr=ADD_QTYPE,
        doc='add docstring',
    )

  @parameterized.parameters(*EVAL_TEST_CASES)
  def test_literal_eval(self, fn, args, expected_res):
    op = py_function_operator_qvalues.PyFunctionOperator(
        'test.foo',
        inspect.signature(fn),
        arolla_abc.PyObject(fn),
        qtype_inference_expr=expected_res.qtype,
    )
    arolla_testing.assert_qvalue_allequal(boxing.eval_(op(*args)), expected_res)

  def test_leaf_eval(self):
    expr = self.add_op(l_x, l_y)
    arolla_testing.assert_qvalue_allequal(
        boxing.eval_(expr, x=1, y=2), scalar_qtypes.int32(3)
    )

  def test_nested_op_eval(self):
    # Asserts that we can call PyFunctionOperator during eval.
    call_add_op = py_function_operator_qvalues.PyFunctionOperator(
        'test.call_add',
        'x, y',
        arolla_abc.PyObject(lambda x, y: boxing.eval_(self.add_op(x, y))),
        qtype_inference_expr=ADD_QTYPE,
    )
    expr = call_add_op(l_x, l_y)
    arolla_testing.assert_qvalue_allequal(
        boxing.eval_(expr, x=1, y=2), scalar_qtypes.int32(3)
    )

  def test_nested_qtype_inference_eval(self):
    # Asserts that we can use PyFunctionOperator in the qtype_inference_expr.
    qtype_op = py_function_operator_qvalues.PyFunctionOperator(
        'test.qtype',
        'x, y',
        arolla_abc.PyObject(
            lambda x, y: casting.common_qtype(x, y)  # pylint: disable=unnecessary-lambda
        ),
        qtype_inference_expr=p_x,
    )
    call_add_op = py_function_operator_qvalues.PyFunctionOperator(
        'test.nested_qtype',
        'x, y',
        arolla_abc.PyObject(add),
        qtype_inference_expr=qtype_op(p_x, p_y),
    )
    expr = call_add_op(l_x, l_y)
    arolla_testing.assert_qvalue_allequal(
        boxing.eval_(expr, x=1, y=2), scalar_qtypes.int32(3)
    )

  def test_get_py_eval_fn_error(self):
    op = lambda_operator_qvalues.LambdaOperator(0)
    with self.assertRaisesWithLiteralMatch(
        TypeError, f'expected PyFunctionOperator, got {op!r}'
    ):
      clib.get_py_function_operator_py_eval_fn(op)

  def test_get_eval_fn(self):
    eval_fn = self.add_op.get_eval_fn()
    self.assertIsInstance(eval_fn, arolla_abc.PyObject)
    self.assertIs(eval_fn.py_value(), add)

  def test_non_qvalue_return_raises(self):
    op = py_function_operator_qvalues.PyFunctionOperator(
        'test.foo',
        'x',
        arolla_abc.PyObject(lambda x: 1),
        qtype_inference_expr=p_x,
    )
    expr = op(l_x)
    raised_exception = None
    try:
      _ = boxing.eval_(expr, x=1)
    except ValueError as e:
      raised_exception = e
    self.assertIsNotNone(raised_exception)
    self.assertRegex(
        str(raised_exception),
        re.escape(
            'error when unpacking the evaluation result of'
            ' PyFunctionOperator[test.foo]'
        ),
    )
    self.assertRegex(
        str(raised_exception.__cause__), 'expected QValue, got int'
    )

  def test_incorrect_qtype_return(self):
    op = py_function_operator_qvalues.PyFunctionOperator(
        'test.foo',
        'x, y',
        arolla_abc.PyObject(lambda x, y: x + y),
        qtype_inference_expr=scalar_qtypes.INT32,
    )
    expr = op(1.0, 2.0)
    with self.assertRaisesRegex(
        ValueError, 'expected the result to have qtype INT32, got FLOAT32'
    ):
      _ = boxing.eval_(expr)

  def test_eval_fn_refcount(self):
    # Define the functions locally to keep the refcount hermetic.
    def foo(x, y):
      return x + y

    start_cnt = sys.getrefcount(foo)
    op = py_function_operator_qvalues.PyFunctionOperator(
        'test.foo',
        'x, y',
        arolla_abc.PyObject(foo),
        qtype_inference_expr=ADD_QTYPE,
    )
    # One for holding the fn ptr (in the PyObject) and one for the wrapped
    # EvalFn.
    self.assertEqual(sys.getrefcount(foo), start_cnt + 2)
    # Each output_qtype_fn increases the refcount by 1.
    eval_fn = op.get_eval_fn().py_value()
    self.assertEqual(sys.getrefcount(foo), start_cnt + 3)
    eval_fn_2 = op.get_eval_fn().py_value()
    self.assertEqual(sys.getrefcount(foo), start_cnt + 4)
    # And deleting reduces the refcount.
    del eval_fn
    gc.collect()
    self.assertEqual(sys.getrefcount(foo), start_cnt + 3)
    del eval_fn_2
    gc.collect()
    self.assertEqual(sys.getrefcount(foo), start_cnt + 2)
    # Binding adds no reference.
    expr = op(1, 2)
    self.assertEqual(sys.getrefcount(foo), start_cnt + 2)
    # Eval adds no reference. We avoid arolla.eval due to its LRU cache which
    # keeps the reference alive. See b/257262359 for more info.
    compiled_expr = arolla_abc.CompiledExpr(expr, {})
    _ = compiled_expr()
    self.assertEqual(sys.getrefcount(foo), start_cnt + 2)
    # Deleting the op and expr will reset the refcount to where we started.
    del expr
    del op
    gc.collect()
    self.assertEqual(sys.getrefcount(foo), start_cnt)


class PyFunctionOperatorExceptionPassthroughTest(parameterized.TestCase):

  def test_compile_error(self):
    def my_fn1(x):
      del x
      raise NotImplementedError('not yet implemented')

    op = py_function_operator_qvalues.PyFunctionOperator(
        'test.foo',
        'x',
        arolla_abc.PyObject(my_fn1),
        qtype_inference_expr=p_x,
    )
    expr = op(1)
    e = None

    def my_fn2():
      return boxing.eval_(expr)

    try:
      my_fn2()
    except NotImplementedError as ex:
      e = ex
    self.assertIsInstance(e, NotImplementedError)
    self.assertRegex(str(e), 'not yet implemented')
    self.assertIn('in my_fn1', ''.join(traceback.format_exception(e)))
    self.assertIn('in my_fn2', ''.join(traceback.format_exception(e)))

  def test_compile_error_chained(self):
    def my_outer_fn(x):
      del x

      def my_fn(x):
        del x
        raise NotImplementedError('not yet implemented')

      op = py_function_operator_qvalues.PyFunctionOperator(
          'test.foo',
          'x',
          arolla_abc.PyObject(my_fn),
          qtype_inference_expr=p_x,
      )
      expr = op(1)
      return boxing.eval_(expr)

    op = py_function_operator_qvalues.PyFunctionOperator(
        'test.foo_outer',
        'x',
        arolla_abc.PyObject(my_outer_fn),
        qtype_inference_expr=p_x,
    )
    expr = op(1)
    e = None
    try:
      boxing.eval_(expr)
    except NotImplementedError as ex:
      e = ex
    self.assertIsNotNone(e)
    self.assertIsInstance(e, NotImplementedError)  # pytype: disable=attribute-error
    self.assertRegex(str(e), 'not yet implemented')  # pytype: disable=attribute-error
    self.assertIn('in my_fn', ''.join(traceback.format_exception(e)))
    self.assertIn('in my_outer_fn', ''.join(traceback.format_exception(e)))

  def test_eval_error(self):
    def my_fn(x):
      del x
      raise NotImplementedError('not yet implemented')

    op = py_function_operator_qvalues.PyFunctionOperator(
        'test.foo',
        'x',
        arolla_abc.PyObject(my_fn),
        qtype_inference_expr=p_x,
    )
    expr = op(l_x)
    e = None
    try:
      boxing.eval_(expr, x=1)
    except NotImplementedError as ex:
      e = ex
    self.assertIsInstance(e, NotImplementedError)
    self.assertRegex(str(e), 'not yet implemented')
    self.assertIn('in my_fn', ''.join(traceback.format_exception(e)))

  def test_eval_error_chained(self):
    def my_outer_fn(x):
      def my_fn(x):
        del x
        raise NotImplementedError('not yet implemented')

      op = py_function_operator_qvalues.PyFunctionOperator(
          'test.foo',
          'x',
          arolla_abc.PyObject(my_fn),
          qtype_inference_expr=p_x,
      )
      expr = op(l_x)
      return boxing.eval_(expr, x=x)

    op = py_function_operator_qvalues.PyFunctionOperator(
        'test.foo_outer',
        'x',
        arolla_abc.PyObject(my_outer_fn),
        qtype_inference_expr=p_x,
    )
    expr = op(l_x)
    e = None
    try:
      boxing.eval_(expr, x=1)
    except NotImplementedError as ex:
      e = ex
    self.assertIsInstance(e, NotImplementedError)  # pytype: disable=attribute-error
    self.assertRegex(str(e), 'not yet implemented')  # pytype: disable=attribute-error
    self.assertIn('in my_fn', ''.join(traceback.format_exception(e)))
    self.assertIn('in my_outer_fn', ''.join(traceback.format_exception(e)))


class PyFunctionOperatorQValueQTypeInferenceTest(parameterized.TestCase):

  def setUp(self):
    super().setUp()
    self.add_op = py_function_operator_qvalues.PyFunctionOperator(
        'test.add',
        'x, y',
        arolla_abc.PyObject(add),
        qtype_inference_expr=ADD_QTYPE,
        doc='add docstring',
    )

  @parameterized.parameters(
      (
          scalar_qtypes.float32(1.0),
          scalar_qtypes.float64(2.0),
          scalar_qtypes.FLOAT64,
      ),
      (
          scalar_qtypes.float32(1.0),
          scalar_qtypes.float64(2.0),
          scalar_qtypes.FLOAT64,
      ),
  )
  def test_qtype(self, x, y, expected_qtype):
    node = self.add_op(x, y)
    self.assertEqual(node.qtype, expected_qtype)

  def test_partial_infer_attr_success(self):
    attr = arolla_abc.infer_attr(
        self.add_op, (scalar_qtypes.FLOAT32, arolla_abc.Attr())
    )
    self.assertIsNone(attr.qtype)
    self.assertIsNone(attr.qvalue)

  def test_varargs_qtype(self):
    def fn(x, *args):
      return boxing.tuple_(x, args)

    ranking_fn = py_function_operator_qvalues.PyFunctionOperator(
        'test.ranking_fn',
        'x, *args',
        arolla_abc.PyObject(fn),
        qtype_inference_expr=make_tuple_qtype_op(p_x, p_args),
        doc='add docstring',
    )
    node = ranking_fn(
        scalar_qtypes.float32(1.0),
        scalar_qtypes.float64(2.0),
        scalar_qtypes.int32(3),
    )
    self.assertEqual(
        node.qtype,
        tuple_qtypes.make_tuple_qtype(
            scalar_qtypes.FLOAT32,
            tuple_qtypes.make_tuple_qtype(
                scalar_qtypes.FLOAT64, scalar_qtypes.INT32
            ),
        ),
    )

  def test_qtype_error(self):
    with self.assertRaisesRegex(
        ValueError,
        re.escape('qtype inference expression produced no qtype'),
    ):
      _ = self.add_op(True, 123)

  def test_get_qtype_inference_expr_error(self):
    op = lambda_operator_qvalues.LambdaOperator(0)
    with self.assertRaisesWithLiteralMatch(
        TypeError, f'expected PyFunctionOperator, got {op!r}'
    ):
      clib.get_py_function_operator_qtype_inference_expr(op)

  def test_get_qtype_inference_expr(self):
    qtype_inference_expr = self.add_op.get_qtype_inference_expr()
    arolla_testing.assert_expr_equal_by_fingerprint(
        qtype_inference_expr, ADD_QTYPE
    )


class PyFunctionOperatorQValueQTypeConstraintsTest(parameterized.TestCase):

  def setUp(self):
    super().setUp()
    self.add_op = py_function_operator_qvalues.PyFunctionOperator(
        'test.add',
        'x, y',
        arolla_abc.PyObject(add),
        qtype_inference_expr=ADD_QTYPE,
        qtype_constraints=[
            (is_numeric_qtype(p_x), 'expected a numeric, got x: {x}'),
            (is_numeric_qtype(p_y), 'expected a numeric, got y: {y}'),
        ],
        doc='add docstring',
    )

  @parameterized.parameters(
      (scalar_qtypes.float32(1.0), scalar_qtypes.float64(2.0)),
      (scalar_qtypes.int32(1), scalar_qtypes.float64(2.0)),
  )
  def test_success(self, x, y):
    _ = self.add_op(x, y)

  @parameterized.parameters(
      (
          scalar_qtypes.unit(),
          scalar_qtypes.float64(2.0),
          'expected a numeric, got x: UNIT',
      ),
      (
          scalar_qtypes.int32(1),
          scalar_qtypes.unit(),
          'expected a numeric, got y: UNIT',
      ),
  )
  def test_failure(self, x, y, expected_error_msg):
    with self.assertRaisesRegex(ValueError, expected_error_msg):
      _ = self.add_op(x, y)

  def test_get_qtype_constraints_error(self):
    op = lambda_operator_qvalues.LambdaOperator(0)
    with self.assertRaisesWithLiteralMatch(
        TypeError, f'expected PyFunctionOperator, got {op!r}'
    ):
      clib.get_py_function_operator_qtype_constraints(op)

  def test_get_qtype_constraints(self):
    constraints = self.add_op.get_qtype_constraints()
    self.assertLen(constraints, 2)
    arolla_testing.assert_expr_equal_by_fingerprint(
        constraints[0][0], is_numeric_qtype(p_x)
    )
    self.assertEqual(constraints[0][1], 'expected a numeric, got x: {x}')
    arolla_testing.assert_expr_equal_by_fingerprint(
        constraints[1][0], is_numeric_qtype(p_y)
    )
    self.assertEqual(constraints[1][1], 'expected a numeric, got y: {y}')

  def test_partial_infer_attr_early_failure(self):
    with self.assertRaisesRegex(ValueError, 'expected a numeric, got x: UNIT'):
      arolla_abc.infer_attr(
          self.add_op, (scalar_qtypes.UNIT, arolla_abc.Attr())
      )


if __name__ == '__main__':
  absltest.main()
