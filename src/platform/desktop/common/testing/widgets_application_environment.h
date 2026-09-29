#pragma once

#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <QApplication>

namespace fastecu::testing
{
using WidgetsApplicationEnvironment = ApplicationEnvironment<QApplication>;
}
