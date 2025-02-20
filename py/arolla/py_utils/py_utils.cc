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
#include "py/arolla/py_utils/py_utils.h"

#include <Python.h>

#include <cstdarg>
#include <cstddef>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/cord.h"
#include "absl/strings/string_view.h"
#include "py/arolla/py_utils/py_object_as_status_payload.h"
#include "py/arolla/py_utils/status_payload_handler_registry.h"
#include "arolla/util/init_arolla.h"
#include "arolla/util/status_macros_backport.h"

namespace arolla::python {
namespace {
std::string StatusToString(const absl::Status& status) {
  std::ostringstream message;

  // Include the status code, unless it's StatusCode::kInvalidArgument.
  if (status.code() != absl::StatusCode::kInvalidArgument) {
    message << "[" << absl::StatusCodeToString(status.code()) << "]";
  }
  if (auto status_message = absl::StripAsciiWhitespace(status.message());
      !status_message.empty()) {
    if (message.tellp() > 0) {
      message << ' ';
    }
    message << status_message;
  }

  return message.str();
}

// If payload contains a Python exception, raise it here. Otherwise, convert
// Status to ValueError and raise.
void HandlePythonExceptionPayload(absl::Cord payload,
                                  const absl::Status& status) {
  auto py_object_ptr =
      UnwrapPyObjectFromCord(std::move(payload)).value_or(PyObjectGILSafePtr());
  if (py_object_ptr == nullptr) {
    DefaultSetPyErrFromStatus(status);
    return;
  }
  PyObject* py_exception = py_object_ptr.get();
  PyErr_SetObject((PyObject*)Py_TYPE(py_exception), py_exception);
}

// If payload contains a Python exception cause, first turn the Status into
// ValueError and then attach the cause.
void HandlePythonExceptionCausePayload(absl::Cord payload,
                                       const absl::Status& status) {
  auto py_object_ptr =
      UnwrapPyObjectFromCord(std::move(payload)).value_or(PyObjectGILSafePtr());
  if (py_object_ptr == nullptr) {
    DefaultSetPyErrFromStatus(status);
    return;
  }
  std::string message = StatusToString(status);
  PyErr_SetString(PyExc_ValueError, std::move(message).c_str());

  PyObjectPtr ptype, pvalue, ptraceback;
  PyErr_Fetch(&ptype, &pvalue, &ptraceback);
  PyErr_NormalizeException(&ptype, &pvalue, &ptraceback);
  DCHECK(pvalue != nullptr);
  PyException_SetCause(pvalue.get(), py_object_ptr.release());
  PyErr_Restore(ptype.release(), pvalue.release(), ptraceback.release());
}
}  // namespace

void DefaultSetPyErrFromStatus(const absl::Status& status) {
  std::string message = StatusToString(status);

  PyErr_SetString(PyExc_ValueError, std::move(message).c_str());
}

std::nullptr_t SetPyErrFromStatus(const absl::Status& status) {
  DCheckPyGIL();
  DCHECK(!status.ok());

  std::vector<std::string> type_urls;
  status.ForEachPayload(
      [&type_urls](absl::string_view type_url, const absl::Cord& payload) {
        type_urls.push_back(std::string(type_url));
      });

  if (type_urls.size() == 1) {
    StatusPayloadHandler handler = GetStatusHandlerOrNull(type_urls.front());
    if (handler != nullptr) {
      std::optional<absl::Cord> payload = status.GetPayload(type_urls.front());
      handler(*std::move(payload), status);
      return nullptr;
    }
  }

  // Otherwise, convert Status to ValueError and raise.
  DefaultSetPyErrFromStatus(status);
  return nullptr;
}

namespace {
absl::Status WrapPyErrToStatus(absl::StatusCode code, absl::string_view message,
                               absl::string_view payload_type_url) {
  DCheckPyGIL();

  // Fetch and normalize the python exception.
  PyObjectPtr ptype, pvalue, ptraceback;
  PyErr_Fetch(&ptype, &pvalue, &ptraceback);
  if (ptype == nullptr) {
    return absl::OkStatus();
  }
  PyErr_NormalizeException(&ptype, &pvalue, &ptraceback);
  if (ptraceback != nullptr) {
    PyException_SetTraceback(pvalue.get(), ptraceback.get());
  }

  // Build absl::Status.
  absl::Status status(code, message);
  WritePyObjectToStatusPayload(&status, payload_type_url,
                               PyObjectGILSafePtr::Own(pvalue.release()))
      .IgnoreError();
  return status;
}
}  // namespace

absl::Status StatusCausedByPyErr(absl::StatusCode code,
                                 absl::string_view message) {
  return WrapPyErrToStatus(code, message, kPyExceptionCause);
}

absl::Status StatusWithRawPyErr(absl::StatusCode code,
                                absl::string_view message) {
  return WrapPyErrToStatus(code, message, kPyException);
}

void PyErr_Fetch(PyObjectPtr* ptype, PyObjectPtr* pvalue,
                 PyObjectPtr* ptraceback) {
  DCheckPyGIL();

  PyObject *ptype_tmp, *pvalue_tmp, *ptraceback_tmp;
  PyErr_Fetch(&ptype_tmp, &pvalue_tmp, &ptraceback_tmp);
  *ptype = PyObjectPtr::Own(ptype_tmp);
  *pvalue = PyObjectPtr::Own(pvalue_tmp);
  *ptraceback = PyObjectPtr::Own(ptraceback_tmp);
}

void PyErr_NormalizeException(PyObjectPtr* ptype, PyObjectPtr* pvalue,
                              PyObjectPtr* ptraceback) {
  DCheckPyGIL();

  auto* ptype_tmp = ptype->release();
  auto* pvalue_tmp = pvalue->release();
  auto* ptraceback_tmp = ptraceback->release();

  PyErr_NormalizeException(&ptype_tmp, &pvalue_tmp, &ptraceback_tmp);

  *ptype = PyObjectPtr::Own(ptype_tmp);
  *pvalue = PyObjectPtr::Own(pvalue_tmp);
  *ptraceback = PyObjectPtr::Own(ptraceback_tmp);
}

PyObjectPtr PyType_LookupMemberOrNull(PyTypeObject* py_type,
                                      PyObject* py_str_attr) {
  DCheckPyGIL();
  // Note: We use the _PyType_Lookup() function for efficiency, even though it
  // is technically private. This function is used in multiple projects,
  // including PyBind11, so we consider it to be safe and stable.
  return PyObjectPtr::NewRef(_PyType_Lookup(py_type, py_str_attr));
}

PyObjectPtr PyObject_BindMember(PyObjectPtr&& py_member, PyObject* self) {
  DCheckPyGIL();
  // If the `member` object has a method `__get__`, we follow the Python
  // descriptor protocol. See:
  // https://docs.python.org/3/howto/descriptor.html#functions-and-methods
  PyTypeObject* py_type_member = Py_TYPE(py_member.get());
  if (py_type_member->tp_descr_get != nullptr) {
    return PyObjectPtr::Own(py_type_member->tp_descr_get(
        py_member.get(), self, reinterpret_cast<PyObject*>(Py_TYPE(self))));
  }
  return std::move(py_member);
}

PyObjectPtr PyObject_CallMember(PyObjectPtr&& py_member, PyObject* self,
                                PyObject* args, PyObject* kwargs) {
  DCheckPyGIL();
  auto py_attr = PyObject_BindMember(std::move(py_member), self);
  if (py_attr == nullptr) {
    return PyObjectPtr{};
  }
  return PyObjectPtr::Own(PyObject_Call(py_attr.get(), args, kwargs));
}

PyObjectPtr PyObject_VectorcallMember(PyObjectPtr&& py_member, PyObject** args,
                                      Py_ssize_t nargsf, PyObject* kwnames) {
  DCheckPyGIL();
  const auto nargs = PyVectorcall_NARGS(nargsf);
  if (nargs == 0) {
    PyErr_SetString(PyExc_TypeError, "no arguments provided");
    return PyObjectPtr{};
  }
  PyTypeObject* py_type_member = Py_TYPE(py_member.get());
  if (PyType_HasFeature(py_type_member, Py_TPFLAGS_METHOD_DESCRIPTOR)) {
    return PyObjectPtr::Own(
        PyObject_Vectorcall(py_member.get(), args, nargsf, kwnames));
  }
  auto py_attr = PyObject_BindMember(std::move(py_member), args[0]);
  if (py_attr == nullptr) {
    return PyObjectPtr{};
  }
  return PyObjectPtr::Own(PyObject_Vectorcall(
      py_attr.get(), args + 1, (nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET,
      kwnames));
}

PyObject* PyErr_FormatFromCause(PyObject* py_exc, const char* format, ...) {
  DCheckPyGIL();
  PyObjectPtr cause_ptype, cause_pvalue, cause_ptraceback;
  DCHECK(PyErr_Occurred());
  PyErr_Fetch(&cause_ptype, &cause_pvalue, &cause_ptraceback);
  if (cause_ptype != nullptr) {  // Always happens because of the DCHECK above.
    PyErr_NormalizeException(&cause_ptype, &cause_pvalue, &cause_ptraceback);
    if (cause_ptraceback != nullptr) {
      PyException_SetTraceback(cause_pvalue.get(), cause_ptraceback.get());
    }
    DCHECK(cause_pvalue != nullptr);
  }
  va_list vargs;
  va_start(vargs, format);
  PyErr_FormatV(py_exc, format, vargs);
  va_end(vargs);
  if (cause_pvalue != nullptr) {
    PyObjectPtr ptype, pvalue, ptraceback;
    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
    PyErr_NormalizeException(&ptype, &pvalue, &ptraceback);
    PyException_SetCause(pvalue.get(), Py_NewRef(cause_pvalue.get()));
    PyException_SetContext(pvalue.get(), cause_pvalue.release());
    PyErr_Restore(ptype.release(), pvalue.release(), ptraceback.release());
  }
  return nullptr;
}

AROLLA_INITIALIZER(.init_fn = []() -> absl::Status {
  RETURN_IF_ERROR(RegisterStatusHandler(kPyExceptionCause,
                                        HandlePythonExceptionCausePayload));
  RETURN_IF_ERROR(
      RegisterStatusHandler(kPyException, HandlePythonExceptionPayload));
  return absl::OkStatus();
})

extern "C" int arolla_python_unsafe_internal_PyErr_CanCallCheckSignal(void);

bool PyErr_CanCallCheckSignal() {
  DCheckPyGIL();
  return arolla_python_unsafe_internal_PyErr_CanCallCheckSignal();
}

}  // namespace arolla::python
