import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: panel
    objectName: "connectionPanel"
    padding: 16

    readonly property bool connecting: dashboardConnection.state === 1
    readonly property bool disconnecting: dashboardConnection.state === 5
    readonly property bool transitioning: connecting || disconnecting
    readonly property bool connected: dashboardConnection.canDisconnect && !connecting

    background: Rectangle {
        color: "#111827"
        border.color: "#334155"
        border.width: 1
        radius: 8
    }

    contentItem: ColumnLayout {
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3

                Label {
                    text: qsTr("Connection")
                    color: "#f8fafc"
                    font.pixelSize: 17
                    font.bold: true
                }

                Label {
                    objectName: "connectionStatus"
                    text: dashboardConnection.statusText
                    color: "#cbd5e1"
                    font.pixelSize: 14
                }

                Label {
                    objectName: "selectedAdapterLabel"
                    text: dashboardConnection.selectedAdapterLabel.length > 0
                          ? qsTr("Adapter: %1").arg(dashboardConnection.selectedAdapterLabel)
                          : qsTr("Adapter: Not selected")
                    color: "#94a3b8"
                    font.pixelSize: 13
                }
            }

            Button {
                id: primaryAction
                objectName: "connectButton"
                text: panel.connected ? qsTr("Disconnect") : qsTr("Connect")
                enabled: !panel.transitioning
                         && (dashboardConnection.canConnect || dashboardConnection.canDisconnect)
                onClicked: {
                    if (panel.connected)
                        dashboardConnection.disconnectDashboard()
                    else
                        dashboardConnection.connectDashboard()
                }
            }
        }

        RowLayout {
            visible: dashboardConnection.needsAdapterSelection
            Layout.fillWidth: true
            spacing: 8

            ComboBox {
                id: adapterPicker
                objectName: "adapterPicker"
                Layout.fillWidth: true
                model: dashboardConnection.candidates
                textRole: "label"
                valueRole: "candidateId"
            }

            Connections {
                target: dashboardConnection.candidates
                function onModelReset() {
                    adapterPicker.currentIndex = adapterPicker.count > 0 ? 0 : -1
                }
            }

            Button {
                objectName: "confirmAdapterButton"
                text: qsTr("Use adapter")
                enabled: !panel.transitioning && adapterPicker.currentIndex >= 0
                onClicked: dashboardConnection.connectWithAdapter(adapterPicker.currentValue)
            }
        }

        Button {
            objectName: "refreshAdaptersButton"
            text: qsTr("Refresh adapters")
            flat: true
            enabled: !panel.transitioning && !dashboardConnection.canDisconnect
            onClicked: dashboardConnection.refreshAdapters()
        }

        Button {
            id: errorToggle
            objectName: "connectionErrorToggle"
            text: checked ? qsTr("Hide details") : qsTr("Show details")
            visible: dashboardConnection.technicalDetail.length > 0
            checkable: true
            flat: true
        }

        Label {
            objectName: "connectionErrorDetail"
            text: dashboardConnection.technicalDetail
            visible: errorToggle.visible && errorToggle.checked
            Layout.fillWidth: true
            color: "#fca5a5"
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }
    }
}
