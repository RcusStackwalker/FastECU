import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmniHaste.Dashboard 1.0

Frame {
    id: card
    objectName: "numericCard"
    padding: 16

    required property string cardTitleText
    required property string cardValueText
    required property string cardUnitText
    required property int cardReadingState
    required property string cardLastUpdateAgeText

    readonly property int waitingReadingState: DashboardCardModel.Waiting
    readonly property int liveReadingState: DashboardCardModel.Live
    readonly property int staleReadingState: DashboardCardModel.Stale
    readonly property string stateText: {
        if (cardReadingState === liveReadingState)
            return qsTr("Live")
        if (cardReadingState === staleReadingState)
            return qsTr("Stale")
        return qsTr("Waiting")
    }
    readonly property color valueColor: {
        if (cardReadingState === liveReadingState)
            return "#f8fafc"
        if (cardReadingState === staleReadingState)
            return "#fbbf24"
        return "#94a3b8"
    }

    Accessible.name: [cardTitleText, cardValueText, cardUnitText, stateText].join(" ")

    background: Rectangle {
        color: card.cardReadingState === card.staleReadingState ? "#3b2d12" : "#111827"
        border.color: card.cardReadingState === card.staleReadingState ? "#d97706" : "#334155"
        border.width: 1
        radius: 8
    }

    contentItem: ColumnLayout {
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Label {
                objectName: "cardTitle"
                text: card.cardTitleText
                color: "#cbd5e1"
                font.pixelSize: 15
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            Label {
                objectName: "cardState"
                text: card.stateText
                color: card.valueColor
                font.pixelSize: 13
            }
        }

        Label {
            objectName: "cardValue"
            text: card.cardValueText
            color: card.valueColor
            font.pixelSize: 36
            font.bold: true
            horizontalAlignment: Text.AlignRight
            Layout.fillWidth: true
        }

        Label {
            objectName: "cardUnit"
            text: card.cardUnitText
            color: "#94a3b8"
            font.pixelSize: 14
            horizontalAlignment: Text.AlignRight
            Layout.fillWidth: true
        }

        Label {
            objectName: "cardAge"
            text: card.cardLastUpdateAgeText
            visible: card.cardReadingState === card.staleReadingState && text.length > 0
            color: "#fbbf24"
            font.pixelSize: 12
            horizontalAlignment: Text.AlignRight
            Layout.fillWidth: true
        }
    }
}
