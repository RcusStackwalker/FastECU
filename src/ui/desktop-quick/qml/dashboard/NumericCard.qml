import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: card
    objectName: "numericCard"
    padding: 16

    required property string cardTitleText
    required property string cardValueText
    required property string cardUnitText
    required property int cardReadingState
    required property string cardLastUpdateAgeText

    readonly property string stateText: cardReadingState === 1 ? qsTr("Live")
                                                           : cardReadingState === 2 ? qsTr("Stale")
                                                                            : qsTr("Waiting")
    readonly property color valueColor: cardReadingState === 1 ? "#f8fafc"
                                                               : cardReadingState === 2 ? "#fbbf24"
                                                                                : "#94a3b8"

    Accessible.name: [cardTitleText, cardValueText, cardUnitText, stateText].join(" ")

    background: Rectangle {
        color: card.cardReadingState === 2 ? "#3b2d12" : "#111827"
        border.color: card.cardReadingState === 2 ? "#d97706" : "#334155"
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
            visible: card.cardReadingState === 2 && text.length > 0
            color: "#fbbf24"
            font.pixelSize: 12
            horizontalAlignment: Text.AlignRight
            Layout.fillWidth: true
        }
    }
}
