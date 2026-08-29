import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../dashboard"

ApplicationWindow {
    id: root
    objectName: "desktopQuickRoot"
    width: 1280
    height: 800
    minimumWidth: 900
    minimumHeight: 600
    visible: true
    title: qsTr("OmniHaste")
    color: "#0b1018"

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: "navigationRail"
            color: "#111827"
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 18
                Label {
                    text: qsTr("OmniHaste")
                    color: "#f8fafc"
                    font.pixelSize: 22
                    font.bold: true
                }
                Button {
                    objectName: "dashboardNavigation"
                    text: qsTr("Dashboard")
                    Layout.fillWidth: true
                }
                Item { Layout.fillHeight: true }
            }
        }

        Rectangle {
            objectName: "workspace"
            color: root.color
            Layout.fillWidth: true
            Layout.fillHeight: true
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 28
                spacing: 16
                DashboardView {
                    presentation: dashboardPresentation
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
                ConnectionPanel {
                    Layout.fillWidth: true
                }
            }
        }
    }
}
