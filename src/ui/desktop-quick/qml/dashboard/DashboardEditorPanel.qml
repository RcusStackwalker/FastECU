import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmniHaste.Dashboard 1.0

Frame {
    id: panel
    objectName: "dashboardEditorPanel"
    Layout.preferredWidth: 340
    Layout.fillHeight: true
    padding: 16

    readonly property string disabledReason: qsTr("Disconnect to edit the dashboard")
    readonly property bool hasSelection: dashboardEditor.selectedCardId.length > 0

    background: Rectangle {
        color: "#111827"
        border.color: "#334155"
        border.width: 1
        radius: 8
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            text: qsTr("Dashboard editor")
            color: "#f8fafc"
            font.pixelSize: 18
            font.bold: true
            Layout.fillWidth: true
        }

        Label {
            text: dashboardDocuments.displayName.length > 0
                  ? dashboardDocuments.displayName : qsTr("No dashboard open")
            color: "#cbd5e1"
            elide: Text.ElideMiddle
            Layout.fillWidth: true
        }

        Label {
            objectName: "dashboardDirtyIndicator"
            text: qsTr("Unsaved changes")
            visible: dashboardDocuments.dirty
            color: "#fbbf24"
            font.bold: true
            Layout.fillWidth: true
        }

        GridLayout {
            columns: 2
            columnSpacing: 8
            rowSpacing: 8
            Layout.fillWidth: true

            Button {
                objectName: "importDashboardButton"
                property string disabledReason: panel.disabledReason
                text: qsTr("Import CDBG")
                enabled: dashboardDocuments.editingEnabled
                ToolTip.visible: hovered && !enabled
                ToolTip.text: disabledReason
                Layout.fillWidth: true
                onClicked: dashboardDocuments.requestImport()
            }

            Button {
                objectName: "openDashboardButton"
                property string disabledReason: panel.disabledReason
                text: qsTr("Open")
                enabled: dashboardDocuments.editingEnabled
                ToolTip.visible: hovered && !enabled
                ToolTip.text: disabledReason
                Layout.fillWidth: true
                onClicked: dashboardDocuments.requestOpen()
            }

            Button {
                objectName: "saveDashboardButton"
                property string disabledReason: panel.disabledReason
                text: qsTr("Save")
                enabled: dashboardDocuments.editingEnabled && dashboardDocuments.hasDocument
                         && dashboardDocuments.dirty
                ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                ToolTip.text: disabledReason
                Layout.fillWidth: true
                onClicked: dashboardDocuments.requestSave()
            }

            Button {
                objectName: "saveAsDashboardButton"
                property string disabledReason: panel.disabledReason
                text: qsTr("Save As")
                enabled: dashboardDocuments.editingEnabled && dashboardDocuments.hasDocument
                ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                ToolTip.text: disabledReason
                Layout.fillWidth: true
                onClicked: dashboardDocuments.requestSaveAs()
            }
        }

        Label {
            text: qsTr("Cards")
            color: "#f8fafc"
            font.bold: true
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            ComboBox {
                id: addCardChannelCombo
                objectName: "addCardChannelCombo"
                property string disabledReason: panel.disabledReason
                model: dashboardEditor.channelChoices
                textRole: "name"
                valueRole: "id"
                enabled: dashboardDocuments.editingEnabled && dashboardEditor.canAdd
                ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                ToolTip.text: disabledReason
                Layout.fillWidth: true
                onActivated: dashboardEditor.setAddChannel(currentValue)

                Binding {
                    target: addCardChannelCombo
                    property: "currentIndex"
                    value: addCardChannelCombo.indexOfValue(dashboardEditor.addChannelId)
                }
            }

            ComboBox {
                id: addCardConversionCombo
                objectName: "addCardConversionCombo"
                property string disabledReason: panel.disabledReason
                model: dashboardEditor.addConversionChoices
                textRole: "unit"
                valueRole: "id"
                enabled: dashboardDocuments.editingEnabled && dashboardEditor.canAdd
                         && addCardChannelCombo.currentIndex >= 0
                ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                ToolTip.text: disabledReason
                Layout.fillWidth: true
            }
        }

        ListView {
            id: cardList
            objectName: "editorCardList"
            model: dashboardEditor
            clip: true
            spacing: 4
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 160)
            Layout.minimumHeight: 72

            delegate: ItemDelegate {
                required property string cardId
                required property string title
                required property string channelName
                required property string conversionId
                width: cardList.width
                text: title.length > 0 ? title : channelName
                highlighted: cardId === dashboardEditor.selectedCardId
                onClicked: dashboardEditor.selectCard(cardId)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Button {
                objectName: "addCardButton"
                property string disabledReason: panel.disabledReason
                text: qsTr("Add")
                enabled: dashboardEditor.canAdd && addCardChannelCombo.currentIndex >= 0
                         && addCardConversionCombo.currentIndex >= 0
                ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                ToolTip.text: disabledReason
                onClicked: dashboardEditor.addCard(addCardChannelCombo.currentValue,
                                                   addCardConversionCombo.currentValue)
            }

            Button {
                objectName: "removeCardButton"
                property string disabledReason: panel.disabledReason
                text: qsTr("Remove")
                enabled: dashboardEditor.canRemove
                ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                ToolTip.text: disabledReason
                onClicked: dashboardEditor.removeSelected()
            }

            Item { Layout.fillWidth: true }

            Button {
                objectName: "moveCardUpButton"
                property string disabledReason: panel.disabledReason
                text: qsTr("Move Up")
                Accessible.name: qsTr("Move selected card up")
                enabled: dashboardEditor.canMoveUp
                ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                ToolTip.text: disabledReason
                onClicked: dashboardEditor.moveSelectedUp()
            }

            Button {
                objectName: "moveCardDownButton"
                property string disabledReason: panel.disabledReason
                text: qsTr("Move Down")
                Accessible.name: qsTr("Move selected card down")
                enabled: dashboardEditor.canMoveDown
                ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                ToolTip.text: disabledReason
                onClicked: dashboardEditor.moveSelectedDown()
            }
        }

        Label {
            visible: !dashboardDocuments.editingEnabled
            text: panel.disabledReason
            color: "#fbbf24"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        ScrollView {
            id: selectedCardForm
            clip: true
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                width: selectedCardForm.availableWidth
                spacing: 8

                Label {
                    text: qsTr("Selected card")
                    color: "#f8fafc"
                    font.bold: true
                    Layout.fillWidth: true
                }

                Label { text: qsTr("Channel"); color: "#cbd5e1" }
                ComboBox {
                    id: cardChannelCombo
                    objectName: "cardChannelCombo"
                    property string disabledReason: panel.disabledReason
                    model: dashboardEditor.channelChoices
                    textRole: "name"
                    valueRole: "id"
                    currentIndex: indexOfValue(dashboardEditor.selectedChannelId)
                    enabled: dashboardDocuments.editingEnabled && panel.hasSelection
                    ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                    ToolTip.text: disabledReason
                    Layout.fillWidth: true
                    onActivated: dashboardEditor.setSelectedChannel(currentValue)
                }

                Label { text: qsTr("Conversion"); color: "#cbd5e1" }
                ComboBox {
                    id: cardConversionCombo
                    objectName: "cardConversionCombo"
                    property string disabledReason: panel.disabledReason
                    model: dashboardEditor.conversionChoices
                    textRole: "unit"
                    valueRole: "id"
                    currentIndex: indexOfValue(dashboardEditor.selectedConversionId)
                    enabled: dashboardDocuments.editingEnabled && panel.hasSelection
                    ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                    ToolTip.text: disabledReason
                    Layout.fillWidth: true
                    onActivated: dashboardEditor.setSelectedConversion(currentValue)
                }

                Label { text: qsTr("Title"); color: "#cbd5e1" }
                TextField {
                    objectName: "cardTitleField"
                    property string disabledReason: panel.disabledReason
                    text: dashboardEditor.selectedTitle
                    enabled: dashboardDocuments.editingEnabled && panel.hasSelection
                    ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                    ToolTip.text: disabledReason
                    Layout.fillWidth: true
                    onEditingFinished: dashboardEditor.setSelectedTitle(text)
                }

                Label { text: qsTr("Display type"); color: "#cbd5e1" }
                ComboBox {
                    id: cardDisplayTypeCombo
                    objectName: "cardDisplayTypeCombo"
                    property string disabledReason: panel.disabledReason
                    model: dashboardEditor.displayTypeChoices
                    textRole: "label"
                    valueRole: "value"
                    currentIndex: indexOfValue(dashboardEditor.selectedDisplayType)
                    enabled: dashboardDocuments.editingEnabled && panel.hasSelection
                    ToolTip.visible: hovered && !enabled && !dashboardDocuments.editingEnabled
                    ToolTip.text: disabledReason
                    Layout.fillWidth: true
                    onActivated: dashboardEditor.setSelectedDisplayType(currentValue)
                }

                ColumnLayout {
                    id: gaugeSettings
                    objectName: "gaugeSettings"
                    visible: panel.hasSelection
                             && dashboardEditor.selectedDisplayType === DashboardCardModel.HorizontalGauge
                    Layout.fillWidth: true

                    Label { text: qsTr("Gauge minimum"); color: "#cbd5e1" }
                    TextField {
                        id: gaugeMinimumField
                        objectName: "gaugeMinimumField"
                        property string disabledReason: panel.disabledReason
                        text: dashboardEditor.selectedGaugeMinimum.toString()
                        enabled: dashboardDocuments.editingEnabled
                        ToolTip.visible: hovered && !enabled
                        ToolTip.text: disabledReason
                        Layout.fillWidth: true
                        onEditingFinished: dashboardEditor.setSelectedGaugeBounds(
                                               Number(text), Number(gaugeMaximumField.text),
                                               Number(gaugeStepField.text))
                    }
                    Label { text: qsTr("Gauge maximum"); color: "#cbd5e1" }
                    TextField {
                        id: gaugeMaximumField
                        objectName: "gaugeMaximumField"
                        property string disabledReason: panel.disabledReason
                        text: dashboardEditor.selectedGaugeMaximum.toString()
                        enabled: dashboardDocuments.editingEnabled
                        ToolTip.visible: hovered && !enabled
                        ToolTip.text: disabledReason
                        Layout.fillWidth: true
                        onEditingFinished: dashboardEditor.setSelectedGaugeBounds(
                                               Number(gaugeMinimumField.text), Number(text),
                                               Number(gaugeStepField.text))
                    }
                    Label { text: qsTr("Gauge step"); color: "#cbd5e1" }
                    TextField {
                        id: gaugeStepField
                        objectName: "gaugeStepField"
                        property string disabledReason: panel.disabledReason
                        text: dashboardEditor.selectedGaugeStep.toString()
                        enabled: dashboardDocuments.editingEnabled
                        ToolTip.visible: hovered && !enabled
                        ToolTip.text: disabledReason
                        Layout.fillWidth: true
                        onEditingFinished: dashboardEditor.setSelectedGaugeBounds(
                                               Number(gaugeMinimumField.text),
                                               Number(gaugeMaximumField.text), Number(text))
                    }
                }

                ColumnLayout {
                    id: sparklineSettings
                    objectName: "sparklineSettings"
                    visible: panel.hasSelection
                             && dashboardEditor.selectedDisplayType === DashboardCardModel.Sparkline
                    Layout.fillWidth: true

                    Label { text: qsTr("History (seconds)"); color: "#cbd5e1" }
                    TextField {
                        objectName: "sparklineHistoryField"
                        property string disabledReason: panel.disabledReason
                        text: dashboardEditor.selectedSparklineHistorySeconds.toString()
                        enabled: dashboardDocuments.editingEnabled
                        ToolTip.visible: hovered && !enabled
                        ToolTip.text: disabledReason
                        Layout.fillWidth: true
                        onEditingFinished: dashboardEditor.setSelectedSparklineHistorySeconds(Number(text))
                    }
                }
            }
        }
    }
}
