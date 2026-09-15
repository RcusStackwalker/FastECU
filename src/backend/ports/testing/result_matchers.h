#pragma once

#include <string>

#include <gmock/gmock.h>

#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/error_printers.h"

namespace fastecu::testing
{

// Matches a Result<T> or Status holding a value. On failure the explanation
// carries the Error that was found, which the bare
// ASSERT_TRUE(x.has_value()) it replaces threw away.
MATCHER(IsOk, "")
{
    if (arg.has_value())
    {
        return true;
    }
    *result_listener << "which failed with " << ::testing::PrintToString(arg.error());
    return false;
}

// Result<T> only; a Status has no value to inspect, so use IsOk() there.
MATCHER_P(IsOkAnd, value_matcher, "")
{
    if (!arg.has_value())
    {
        *result_listener << "which failed with " << ::testing::PrintToString(arg.error());
        return false;
    }
    return ::testing::ExplainMatchResult(value_matcher, *arg, result_listener);
}

MATCHER_P(IsErr, kind, "")
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

MATCHER_P2(IsErrWith, kind, detail_matcher, "")
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
    return ::testing::ExplainMatchResult(detail_matcher, error.detail, result_listener);
}

// Renders a matcher's explanation for a value, so a test can assert on the
// text a failure would print.
template <class Value, class Matcher> std::string explainMatch(const Matcher& matcher, const Value& value)
{
    ::testing::StringMatchResultListener listener;
    ::testing::ExplainMatchResult(matcher, value, &listener);
    return listener.str();
}

} // namespace fastecu::testing
