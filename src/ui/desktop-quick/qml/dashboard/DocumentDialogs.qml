import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as QuickDialogs
import OmniHaste.Dashboard 1.0

Item {
    id: dialogs

    function localPath(selectedUrl) {
        let path = decodeURIComponent(selectedUrl.toString().replace(/^file:\/\//, ""))
        if (Qt.platform.os === "windows" && /^\/[A-Za-z]:/.test(path))
            path = path.substring(1)
        return path
    }

    QuickDialogs.FileDialog {
        id: importDialog
        objectName: "importDashboardDialog"
        title: qsTr("Import CDBG dashboard definition")
        fileMode: QuickDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("CDBG logger definitions (*.xml)"), qsTr("All files (*)")]
        onAccepted: dashboardDocuments.completeImportPath(dialogs.localPath(selectedFile))
        onRejected: dashboardDocuments.cancelPathRequest()
    }

    QuickDialogs.FileDialog {
        id: openDialog
        objectName: "openDashboardDialog"
        title: qsTr("Open dashboard")
        fileMode: QuickDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("OmniHaste dashboards (*.ohd)"), qsTr("All files (*)")]
        onAccepted: dashboardDocuments.completeOpenPath(dialogs.localPath(selectedFile))
        onRejected: dashboardDocuments.cancelPathRequest()
    }

    QuickDialogs.FileDialog {
        id: saveAsDialog
        objectName: "saveAsDashboardDialog"
        title: qsTr("Save dashboard as")
        fileMode: QuickDialogs.FileDialog.SaveFile
        defaultSuffix: "ohd"
        nameFilters: [qsTr("OmniHaste dashboards (*.ohd)"), qsTr("All files (*)")]
        onAccepted: dashboardDocuments.completeSavePath(dialogs.localPath(selectedFile))
        onRejected: dashboardDocuments.cancelPathRequest()
    }

    Dialog {
        id: unsavedDialog
        objectName: "unsavedChangesDialog"
        title: qsTr("Unsaved dashboard changes")
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: parent
        width: 420

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                text: qsTr("Save your dashboard changes before continuing?")
                wrapMode: Text.Wrap
                Layout.preferredWidth: 360
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight

                Button {
                    objectName: "saveUnsavedButton"
                    property string disabledReason: qsTr("Disconnect to edit the dashboard")
                    text: qsTr("Save")
                    enabled: dashboardDocuments.editingEnabled
                    ToolTip.visible: hovered && !enabled
                    ToolTip.text: disabledReason
                    onClicked: {
                        unsavedDialog.close()
                        dashboardDocuments.resolveUnsaved(DashboardDocumentController.Save)
                    }
                }
                Button {
                    objectName: "discardUnsavedButton"
                    text: qsTr("Discard")
                    onClicked: {
                        unsavedDialog.close()
                        dashboardDocuments.resolveUnsaved(DashboardDocumentController.Discard)
                    }
                }
                Button {
                    objectName: "cancelUnsavedButton"
                    text: qsTr("Cancel")
                    onClicked: {
                        unsavedDialog.close()
                        dashboardDocuments.resolveUnsaved(DashboardDocumentController.Cancel)
                    }
                }
            }
        }
    }

    Frame {
        id: errorBanner
        objectName: "documentErrorBanner"
        property alias text: errorLabel.text
        visible: false
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 16
        width: Math.min(520, parent.width - 32)
        padding: 12
        z: 20

        background: Rectangle {
            color: "#7f1d1d"
            border.color: "#fca5a5"
            radius: 8
        }

        contentItem: RowLayout {
            spacing: 12

            Label {
                id: errorLabel
                color: "#fef2f2"
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            Button {
                objectName: "dismissDocumentErrorButton"
                text: qsTr("Dismiss")
                onClicked: errorBanner.visible = false
            }
        }
    }

    Connections {
        target: dashboardDocuments

        function onImportPathRequested() {
            importDialog.open()
        }

        function onOpenPathRequested() {
            openDialog.open()
        }

        function onSavePathRequested() {
            saveAsDialog.open()
        }

        function onUnsavedDecisionRequested() {
            unsavedDialog.open()
        }

        function onErrorOccurred(operation, detail, kind) {
            errorLabel.text = qsTr("%1: %2").arg(operation).arg(detail)
            errorBanner.visible = true
        }
    }
}
