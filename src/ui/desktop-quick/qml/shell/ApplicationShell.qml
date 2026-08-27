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
                spacing: 10
                Item { Layout.fillHeight: true }
                Label {
                    text: qsTr("Dashboard")
                    color: "#f8fafc"
                    font.pixelSize: 28
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }
                Label {
                    objectName: "workspaceMessage"
                    text: qsTr("Dashboard support is coming in the next development phase.")
                    color: "#94a3b8"
                    font.pixelSize: 15
                    Layout.alignment: Qt.AlignHCenter
                }
                Item { Layout.fillHeight: true }
                ConnectionPanel {
                    Layout.fillWidth: true
                }
            }
        }
    }
}
