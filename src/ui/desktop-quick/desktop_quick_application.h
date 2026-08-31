#pragma once

class QQmlApplicationEngine;

namespace fastecu::desktop_quick
{

class DashboardConnectionController;
class DashboardController;
class DashboardDocumentController;
class DashboardEditorModel;

bool load_root(QQmlApplicationEngine& engine, DashboardConnectionController& dashboard_connection,
               DashboardController& dashboard_presentation, DashboardDocumentController& dashboard_documents,
               DashboardEditorModel& dashboard_editor);

} // namespace fastecu::desktop_quick
