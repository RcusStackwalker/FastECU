#pragma once

class QQmlApplicationEngine;

namespace fastecu::desktop_quick
{

class DashboardConnectionController;

bool load_root(QQmlApplicationEngine& engine, DashboardConnectionController& dashboard_connection);

} // namespace fastecu::desktop_quick
