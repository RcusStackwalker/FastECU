#pragma once

#include <gmock/gmock.h>

// Shapes one boolean-setter expectation for the data-driven
// configureFailsAtEachRemainingSetterInTurn() tests, which both flash
// transports have: configure() must reach every setter up to and including the
// failing one, and must never reach the setters after it.
//
// `position` is the setter's index in configure()'s specified order and
// `failingIndex` the one the current data row fails. An expectation that can
// never fire carries no action -- Times(0) combined with WillRepeatedly() makes
// Google Mock log "Too many actions specified" for every such line, which
// buries real diagnostics in the same output.
template <typename Expectation> void ExpectSetterAt(Expectation& expectation, int position, int failing_index)
{
    if (failing_index >= position)
    {
        expectation.WillOnce(::testing::Return(failing_index != position));
    }
    else
    {
        expectation.Times(0);
    }
}
