import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: dashboardView
    objectName: "dashboardView"
    clip: true

    required property var presentation

    contentWidth: availableWidth
    contentHeight: content.implicitHeight

    ColumnLayout {
        id: content
        width: dashboardView.availableWidth
        spacing: 16

        Label {
            objectName: "loadErrorText"
            visible: dashboardView.presentation.hasLoadError
            text: dashboardView.presentation.loadErrorText
            color: "#fca5a5"
            font.pixelSize: 15
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        GridLayout {
            id: dashboardGrid
            objectName: "dashboardGrid"
            visible: !dashboardView.presentation.hasLoadError
            Layout.fillWidth: true
            columnSpacing: 16
            rowSpacing: 16

            readonly property int minimumCardWidth: 240
            columns: Math.max(1, Math.floor((width + columnSpacing) /
                                            (minimumCardWidth + columnSpacing)))

            Repeater {
                model: dashboardView.presentation.cards

                delegate: Item {
                    id: cardDelegate
                    required property string title
                    required property string formattedValue
                    required property string unit
                    required property int readingState
                    required property string lastUpdateAgeText

                    Layout.fillWidth: true
                    Layout.preferredWidth: (dashboardGrid.width
                                            - (dashboardGrid.columns - 1) * dashboardGrid.columnSpacing)
                                           / dashboardGrid.columns

                    implicitWidth: numericCard.implicitWidth
                    implicitHeight: numericCard.implicitHeight

                    NumericCard {
                        id: numericCard
                        anchors.fill: parent
                        cardTitleText: cardDelegate.title
                        cardValueText: cardDelegate.formattedValue
                        cardUnitText: cardDelegate.unit
                        cardReadingState: cardDelegate.readingState
                        cardLastUpdateAgeText: cardDelegate.lastUpdateAgeText
                    }
                }
            }
        }
    }
}
