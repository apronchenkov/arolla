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
#include "arolla/qexpr/operators/strings/strings.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "icu4c/source/common/unicode/bytestream.h"
#include "icu4c/source/common/unicode/casemap.h"
#include "icu4c/source/common/unicode/errorcode.h"
#include "icu4c/source/common/unicode/stringoptions.h"
#include "icu4c/source/common/unicode/umachine.h"
#include "icu4c/source/common/unicode/utf8.h"
#include "double-conversion/double-to-string.h"
#include "double-conversion/utils.h"
#include "arolla/memory/optional_value.h"
#include "arolla/qtype/strings/regex.h"
#include "arolla/util/bytes.h"
#include "arolla/util/indestructible.h"
#include "arolla/util/text.h"
#include "arolla/util/unit.h"
#include "re2/re2.h"
#include "arolla/util/status_macros_backport.h"

namespace arolla {
namespace {

absl::Status ValidateUtf8(absl::string_view bytes) {
  if (bytes.size() > size_t{std::numeric_limits<int32_t>::max()}) {
    return absl::UnimplementedError("string is too long to convert to UTF-8");
  }
  int32_t offset = 0;
  while (offset < bytes.size()) {
    UChar32 character;
    int32_t previous_offset = offset;
    U8_NEXT(bytes.data(), offset, bytes.size(), character);
    if (character < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "invalid UTF-8 sequence at position %d", previous_offset));
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<Text> LowerOp::operator()(
    absl::string_view in, std::optional<absl::string_view> locale) const {
  std::string result;
  icu::StringByteSink<std::string> sink(&result);
  icu::ErrorCode error_code;

  const char* locale_ptr = locale.has_value() ? locale->data() : nullptr;
  icu::CaseMap::utf8ToLower(locale_ptr, U_FOLD_CASE_DEFAULT,
                            absl::string_view(in), sink,
                            /* edits = */ nullptr, error_code);
  if (error_code.isFailure()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "utf8ToLower failed with error: %s", error_code.errorName()));
  }
  return Text(result);
}

absl::StatusOr<Text> UpperOp::operator()(
    absl::string_view in, std::optional<absl::string_view> locale) const {
  std::string result;
  icu::StringByteSink<std::string> sink(&result);
  icu::ErrorCode error_code;
  const char* locale_ptr = locale.has_value() ? locale->data() : nullptr;
  icu::CaseMap::utf8ToUpper(locale_ptr, U_FOLD_CASE_DEFAULT,
                            absl::string_view(in), sink,
                            /* edits = */ nullptr, error_code);
  if (error_code.isFailure()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "utf8ToUpper failed with error: %s", error_code.errorName()));
  }
  return Text(result);
}

absl::StatusOr<Text> DecodeOp::operator()(absl::string_view s) const {
  RETURN_IF_ERROR(ValidateUtf8(s));
  return Text(s);
}

absl::StatusOr<std::string> ReplaceOp::operator()(
    absl::string_view s, absl::string_view old_sub, absl::string_view new_sub,
    OptionalValue<int32_t> max_subs) const {
  size_t count = std::numeric_limits<size_t>::max();
  if (max_subs.present && (max_subs.value >= 0)) {
    count = max_subs.value;
  }
  std::string res;
  if (count == 0) {
    return res;
  }
  size_t offset = 0;
  if (old_sub.empty()) {
    // Special handling for empty 'old_sub'.
    absl::StrAppend(&res, new_sub);
    while ((--count > 0) && (offset < s.length())) {
      absl::StrAppend(&res, s.substr(offset, 1), new_sub);
      ++offset;
    }
  } else {
    while (count-- > 0) {
      const size_t start = s.find(old_sub, offset);
      if (start == std::string::npos) break;
      absl::StrAppend(&res, s.substr(offset, start - offset), new_sub);
      offset = start + old_sub.size();
    }
  }
  res.append(s.begin() + offset, s.end());
  return res;
}

OptionalUnit ContainsRegexOp::operator()(const Text& text,
                                         const Regex& regexp) const {
  return OptionalUnit{
      RE2::PartialMatch(absl::string_view(text), regexp.value())};
}

absl::StatusOr<OptionalValue<Text>> ExtractRegexOp::operator()(
    const Text& text, const Regex& regexp) const {
  const auto& re = regexp.value();
  if (re.NumberOfCapturingGroups() != 1) {
    return absl::InvalidArgumentError(
        absl::StrFormat("ExtractRegexOp expected regular expression with "
                        "exactly one capturing group; got `%s` which "
                        "contains %d capturing groups.",
                        re.pattern(), re.NumberOfCapturingGroups()));
  }
  std::string match;
  if (RE2::PartialMatch(text.view(), re, &match)) {
    return Text(match);
  } else {
    return OptionalValue<Text>();
  }
}

template <class T>
Text SignedIntegerToText(T x) {
  return Text(absl::StrFormat("%d", x));
}

Text AsTextOp::operator()(absl::string_view s) const {
  return Text(absl::StrFormat("b'%s'", absl::Utf8SafeCHexEscape(s)));
}
Text AsTextOp::operator()(const Bytes& x) const { return operator()(x.view()); }
Text AsTextOp::operator()(Unit) const { return Text("unit"); }
Text AsTextOp::operator()(int32_t x) const { return SignedIntegerToText(x); }
Text AsTextOp::operator()(int64_t x) const { return SignedIntegerToText(x); }
Text AsTextOp::operator()(uint64_t x) const {
  return Text(absl::StrFormat("%d", x));
}
Text AsTextOp::operator()(bool x) const {
  return x ? Text("true") : Text("false");
}

Text AsTextOp::operator()(float x) const {
  static const Indestructible<double_conversion::DoubleToStringConverter>
      converter(double_conversion::DoubleToStringConverter::NO_FLAGS, "inf",
                "nan",
                /*exponent_character=*/'e',
                /*decimal_in_shortest_low=*/-6, /*decimal_in_shortest_high=*/21,
                /*max_leading_padding_zeroes_in_precision_mode=*/6,
                /*max_trailing_padding_zeroes_in_precision_mode=*/0);
  char buf[128];
  double_conversion::StringBuilder builder(buf, sizeof(buf));
  converter->ToShortestSingle(x, &builder);
  return Text(builder.Finalize());
}

Text AsTextOp::operator()(double x) const {
  static const Indestructible<double_conversion::DoubleToStringConverter>
      converter(double_conversion::DoubleToStringConverter::NO_FLAGS, "inf",
                "nan",
                /*exponent_character=*/'e',
                /*decimal_in_shortest_low=*/-6, /*decimal_in_shortest_high=*/21,
                /*max_leading_padding_zeroes_in_precision_mode=*/6,
                /*max_trailing_padding_zeroes_in_precision_mode=*/0);
  char buf[128];
  double_conversion::StringBuilder builder(buf, sizeof(buf));
  converter->ToShortest(x, &builder);
  return Text(builder.Finalize());
}

Text TextAsTextOp::operator()(absl::string_view s) const { return Text(s); }
Text TextAsTextOp::operator()(const Text& s) const { return s; }

}  // namespace arolla
