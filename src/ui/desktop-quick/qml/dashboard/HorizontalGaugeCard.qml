import QtQuick

DashboardCardFrame {
    id: card
    objectName: "horizontalGaugeCard"
    readonly property bool usesDashboardCardFrame: true

    required property bool hasReading
    required property real numericValue
    required property real minimumValue
    required property real maximumValue
    required property real stepValue
    readonly property real rawFraction: hasReading
        ? (numericValue - minimumValue) / (maximumValue - minimumValue) : 0
    readonly property real normalizedValue: Math.max(0, Math.min(1, rawFraction))
    readonly property int overflowDirection: !hasReading ? 0
        : numericValue < minimumValue ? -1
        : numericValue > maximumValue ? 1 : 0
    readonly property int tickCount: Math.min(12,
        Math.max(1, Math.ceil((maximumValue - minimumValue) / stepValue)))

    visualizationContent: Canvas {
        id: gaugeCanvas
        objectName: "gaugeCanvas"
        anchors.fill: parent
        opacity: card.cardReadingState === card.staleReadingState ? 0.55 : 1.0

        function repaint() {
            requestPaint()
        }

        onWidthChanged: repaint()
        onHeightChanged: repaint()

        onPaint: {
            var context = getContext("2d")
            context.clearRect(0, 0, width, height)
            if (width <= 0 || height <= 0)
                return

            var trackLeft = 8
            var trackRight = width - 8
            var trackWidth = Math.max(0, trackRight - trackLeft)
            var trackY = Math.round(height * 0.45)
            var trackHeight = 10
            context.fillStyle = "#334155"
            context.fillRect(trackLeft, trackY - trackHeight / 2, trackWidth, trackHeight)

            if (card.hasReading) {
                context.fillStyle = "#38bdf8"
                context.fillRect(trackLeft, trackY - trackHeight / 2,
                                 trackWidth * card.normalizedValue, trackHeight)
            }

            context.strokeStyle = "#94a3b8"
            context.lineWidth = 1
            for (var tickIndex = 0; tickIndex < card.tickCount; ++tickIndex) {
                var fraction = card.tickCount === 1 ? 0 : tickIndex / (card.tickCount - 1)
                var tickX = trackLeft + trackWidth * fraction
                context.beginPath()
                context.moveTo(tickX, trackY + trackHeight / 2)
                context.lineTo(tickX, trackY + trackHeight / 2 + 5)
                context.stroke()
            }

            context.fillStyle = "#94a3b8"
            context.font = "11px sans-serif"
            context.textAlign = "left"
            context.fillText(Number(card.minimumValue).toLocaleString(), trackLeft, height - 2)
            context.textAlign = "right"
            context.fillText(Number(card.maximumValue).toLocaleString(), trackRight, height - 2)

            if (card.overflowDirection !== 0) {
                var triangleX = card.overflowDirection < 0 ? trackLeft : trackRight
                var triangleDirection = card.overflowDirection < 0 ? -1 : 1
                context.fillStyle = "#fbbf24"
                context.beginPath()
                context.moveTo(triangleX, trackY - trackHeight / 2 - 3)
                context.lineTo(triangleX + triangleDirection * 7, trackY - trackHeight / 2 - 8)
                context.lineTo(triangleX + triangleDirection * 7, trackY - trackHeight / 2 + 2)
                context.closePath()
                context.fill()
            }
        }
    }

    Connections {
        target: card

        function onHasReadingChanged() { gaugeCanvas.requestPaint() }
        function onNumericValueChanged() { gaugeCanvas.requestPaint() }
        function onMinimumValueChanged() { gaugeCanvas.requestPaint() }
        function onMaximumValueChanged() { gaugeCanvas.requestPaint() }
        function onStepValueChanged() { gaugeCanvas.requestPaint() }
        function onCardReadingStateChanged() { gaugeCanvas.requestPaint() }
    }
}
