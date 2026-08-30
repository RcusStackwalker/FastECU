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
    readonly property bool hasFiniteRange: Number.isFinite(minimumValue)
        && Number.isFinite(maximumValue) && maximumValue > minimumValue
        && Number.isFinite(maximumValue - minimumValue)
    readonly property real range: hasFiniteRange ? maximumValue - minimumValue : 0
    readonly property real stepIntervalCount: hasFiniteRange && Number.isFinite(stepValue) && stepValue > 0
        ? range / stepValue : 0
    readonly property int tickCount: Number.isFinite(stepIntervalCount) && stepIntervalCount > 0
        ? Math.min(12, Math.max(1, Math.ceil(stepIntervalCount))) : 0
    readonly property real tickGuideStrideMultiplier: {
        if (tickCount === 0)
            return 0
        var stride = Math.max(1, Math.ceil(stepIntervalCount / tickCount))
        return Number.isSafeInteger(stride) ? stride : 0
    }
    readonly property real tickGuideStepValue: {
        var renderedStep = stepValue * tickGuideStrideMultiplier
        return Number.isFinite(renderedStep) && renderedStep > 0 ? renderedStep : 0
    }
    readonly property real rawFraction: {
        if (!hasReading || !hasFiniteRange || !Number.isFinite(numericValue))
            return 0
        var fraction = (numericValue - minimumValue) / range
        return Number.isFinite(fraction) ? fraction : 0
    }
    readonly property real normalizedValue: Number.isFinite(rawFraction)
        ? Math.max(0, Math.min(1, rawFraction)) : 0
    readonly property int overflowDirection: !hasReading || !hasFiniteRange || !Number.isFinite(numericValue) ? 0
        : numericValue < minimumValue ? -1
        : numericValue > maximumValue ? 1 : 0
    readonly property real overflowTriangleTipOffset: overflowDirection * 7

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
            if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0)
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

            if (card.tickGuideStepValue > 0) {
                context.strokeStyle = "#94a3b8"
                context.lineWidth = 1
                for (var tickIndex = 0; tickIndex < card.tickCount; ++tickIndex) {
                    var guideValue = card.minimumValue + tickIndex * card.tickGuideStepValue
                    if (!Number.isFinite(guideValue) || guideValue > card.maximumValue)
                        break
                    var guideFraction = (guideValue - card.minimumValue) / card.range
                    var tickX = trackLeft + trackWidth * guideFraction
                    if (!Number.isFinite(guideFraction) || !Number.isFinite(tickX))
                        continue
                    context.beginPath()
                    context.moveTo(tickX, trackY + trackHeight / 2)
                    context.lineTo(tickX, trackY + trackHeight / 2 + 5)
                    context.stroke()
                }
            }

            if (card.hasFiniteRange) {
                context.fillStyle = "#94a3b8"
                context.font = "11px sans-serif"
                context.textAlign = "left"
                context.fillText(Number(card.minimumValue).toLocaleString(), trackLeft, height - 2)
                context.textAlign = "right"
                context.fillText(Number(card.maximumValue).toLocaleString(), trackRight, height - 2)
            }

            if (card.overflowDirection !== 0) {
                var triangleBaseX = card.overflowDirection < 0 ? trackLeft : trackRight
                var triangleTipX = triangleBaseX + card.overflowTriangleTipOffset
                var triangleY = trackY - trackHeight / 2 - 3
                if (!Number.isFinite(triangleTipX) || !Number.isFinite(triangleY))
                    return
                context.fillStyle = "#fbbf24"
                context.beginPath()
                context.moveTo(triangleTipX, triangleY - 5)
                context.lineTo(triangleBaseX, triangleY - 10)
                context.lineTo(triangleBaseX, triangleY)
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
