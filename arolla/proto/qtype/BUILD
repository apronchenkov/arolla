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

# Utilities for proto descriptors and QTypes.

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

cc_library(
    name = "qtype",
    srcs = [
        "proto_qtype_map.cc",
    ],
    hdrs = [
        "proto_qtype_map.h",
    ],
    local_defines = ["AROLLA_IMPLEMENTATION"],
    deps = [
        "//arolla/proto",
        "//arolla/qtype",
        "//arolla/util",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_test(
    name = "proto_qtype_map_test",
    srcs = ["proto_qtype_map_test.cc"],
    deps = [
        ":qtype",
        "//arolla/qtype",
        "//arolla/util",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:status_matchers",
        "@com_google_googletest//:gtest_main",
        "@com_google_protobuf//:protobuf",
    ],
)
