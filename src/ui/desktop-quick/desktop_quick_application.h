#pragma once

class QQmlApplicationEngine;

namespace fastecu::desktop_quick
{

class DashboardConnectionController;
class DashboardController;

bool load_root(QQmlApplicationEngine& engine, DashboardConnectionController& dashboard_connection,
               DashboardController& dashboard_presentation);

} // namespace fastecu::desktop_quick
