import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmniHaste.Dashboard 1.0

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
                    required property int displayType
                    required property real minimumValue
                    required property real maximumValue
                    required property real stepValue
                    required property int sparklineHistorySeconds
                    required property var sparklinePoints
                    required property bool hasReading
                    required property real numericValue

                    Layout.fillWidth: true
                    Layout.preferredWidth: (dashboardGrid.width
                                            - (dashboardGrid.columns - 1) * dashboardGrid.columnSpacing)
                                           / dashboardGrid.columns

                    implicitWidth: loader.item ? loader.item.implicitWidth : 240
                    implicitHeight: loader.item ? loader.item.implicitHeight : 170

                    Loader {
                        id: loader
                        anchors.fill: parent
                        sourceComponent: cardDelegate.displayType === DashboardCardModel.Sparkline
                            ? sparklineComponent
                            : cardDelegate.displayType === DashboardCardModel.HorizontalGauge
                              ? gaugeComponent : numericComponent
                    }

                    Component {
                        id: numericComponent

                        NumericCard {
                            cardTitleText: cardDelegate.title
                            cardValueText: cardDelegate.formattedValue
                            cardUnitText: cardDelegate.unit
                            cardReadingState: cardDelegate.readingState
                            cardLastUpdateAgeText: cardDelegate.lastUpdateAgeText
                        }
                    }

                    Component {
                        id: sparklineComponent

                        SparklineCard {
                            cardTitleText: cardDelegate.title
                            cardValueText: cardDelegate.formattedValue
                            cardUnitText: cardDelegate.unit
                            cardReadingState: cardDelegate.readingState
                            cardLastUpdateAgeText: cardDelegate.lastUpdateAgeText
                            minimumValue: cardDelegate.minimumValue
                            maximumValue: cardDelegate.maximumValue
                            stepValue: cardDelegate.stepValue
                            historySeconds: cardDelegate.sparklineHistorySeconds
                            points: cardDelegate.sparklinePoints
                        }
                    }

                    Component {
                        id: gaugeComponent

                        HorizontalGaugeCard {
                            cardTitleText: cardDelegate.title
                            cardValueText: cardDelegate.formattedValue
                            cardUnitText: cardDelegate.unit
                            cardReadingState: cardDelegate.readingState
                            cardLastUpdateAgeText: cardDelegate.lastUpdateAgeText
                            hasReading: cardDelegate.hasReading
                            numericValue: cardDelegate.numericValue
                            minimumValue: cardDelegate.minimumValue
                            maximumValue: cardDelegate.maximumValue
                            stepValue: cardDelegate.stepValue
                        }
                    }
                }
            }
        }
    }

}
