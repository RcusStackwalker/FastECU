#pragma once

#include <string>
#include <type_traits>

#include <gmock/gmock.h>

#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/error_printers.h"

namespace fastecu::testing
{

// Matches a Result<T> or Status holding a value. On failure the explanation
// carries the Error that was found, which the bare
// ASSERT_TRUE(x.has_value()) it replaces threw away.
MATCHER(IsOk, negation ? "is not ok" : "is ok")
{
    if (arg.has_value())
    {
        return true;
    }
    *result_listener << "which failed with " << ::testing::PrintToString(arg.error());
    return false;
}

// Result<T> only; a Status has no value to inspect, so use IsOk() there.
MATCHER_P(IsOkAnd, value_matcher,
          std::string(negation ? "is not ok, or its value " : "is ok and its value ") +
              ::testing::DescribeMatcher<typename std::remove_cvref_t<arg_type>::value_type>(value_matcher, negation))
{
    if (!arg.has_value())
    {
        *result_listener << "which failed with " << ::testing::PrintToString(arg.error());
        return false;
    }
    ::testing::StringMatchResultListener inner;
    const bool matched = ::testing::ExplainMatchResult(value_matcher, *arg, &inner);
    if (!matched)
    {
        *result_listener << "whose value is " << ::testing::PrintToString(*arg);
        if (!inner.str().empty())
        {
            *result_listener << ", ";
        }
    }
    *result_listener << inner.str();
    return matched;
}

MATCHER_P(IsErr, kind, std::string(negation ? "is not" : "is") + " an error of kind " + ::testing::PrintToString(kind))
{
    if (arg.has_value())
    {
        *result_listener << "which succeeded";
        return false;
    }
    if (arg.error().kind != kind)
    {
        *result_listener << "which failed with " << ::testing::PrintToString(arg.error());
        return false;
    }
    return true;
}

MATCHER_P2(IsErrWith, kind, detail_matcher,
           std::string(negation ? "is not" : "is") + " an error of kind " + ::testing::PrintToString(kind) +
               " whose detail " + ::testing::DescribeMatcher<std::string>(detail_matcher))
{
    if (arg.has_value())
    {
        *result_listener << "which succeeded";
        return false;
    }
    const Error& error = arg.error();
    if (error.kind != kind)
    {
        *result_listener << "which failed with " << ::testing::PrintToString(error);
        return false;
    }
    ::testing::StringMatchResultListener inner;
    const bool matched = ::testing::ExplainMatchResult(detail_matcher, error.detail, &inner);
    if (!matched)
    {
        *result_listener << "whose detail is " << ::testing::PrintToString(error.detail);
        if (!inner.str().empty())
        {
            *result_listener << ", ";
        }
    }
    *result_listener << inner.str();
    return matched;
}

// Any std::expected, including those whose error is not a fastecu::Error:
// matches when it holds an error that satisfies error_matcher.
MATCHER_P(IsErrThat, error_matcher,
          std::string(negation ? "is not an error that " : "is an error that ") +
              ::testing::DescribeMatcher<typename std::remove_cvref_t<arg_type>::error_type>(error_matcher, negation))
{
    if (arg.has_value())
    {
        *result_listener << "which succeeded";
        return false;
    }
    ::testing::StringMatchResultListener inner;
    const bool matched = ::testing::ExplainMatchResult(error_matcher, arg.error(), &inner);
    if (!matched)
    {
        *result_listener << "whose error is " << ::testing::PrintToString(arg.error());
        if (!inner.str().empty())
        {
            *result_listener << ", ";
        }
    }
    *result_listener << inner.str();
    return matched;
}

// Renders a matcher's explanation for a value, so a test can assert on the
// text a failure would print.
template <typename Value, typename Matcher> std::string ExplainMatch(const Matcher& matcher, const Value& value)
{
    ::testing::StringMatchResultListener listener;
    ::testing::ExplainMatchResult(matcher, value, &listener);
    return listener.str();
}

} // namespace fastecu::testing
