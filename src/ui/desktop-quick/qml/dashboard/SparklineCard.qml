import QtQuick

DashboardCardFrame {
    id: card
    objectName: "sparklineCard"
    readonly property bool usesDashboardCardFrame: true

    required property real minimumValue
    required property real maximumValue
    required property real stepValue
    required property int historySeconds
    required property var points
    readonly property int segmentCount: {
        var count = 0
        for (var i = 0; i < points.length; ++i)
            if (points[i].startsSegment)
                ++count
        return count
    }

    visualizationContent: Canvas {
        id: sparklineCanvas
        objectName: "sparklineCanvas"
        anchors.fill: parent
        opacity: card.cardReadingState === card.staleReadingState ? 0.55 : 1.0

        function clamp(value, lower, upper) {
            return Math.max(lower, Math.min(upper, value))
        }

        function repaint() {
            requestPaint()
        }

        onWidthChanged: repaint()
        onHeightChanged: repaint()

        onPaint: {
            var context = getContext("2d")
            context.clearRect(0, 0, width, height)

            var range = card.maximumValue - card.minimumValue
            if (width <= 0 || height <= 0 || range <= 0 || card.historySeconds <= 0 || card.points.length < 2)
                return

            var historyMilliseconds = card.historySeconds * 1000
            var xFor = function(point) {
                return ((point.elapsedMs + historyMilliseconds) / historyMilliseconds) * width
            }
            var yFor = function(value) {
                var fraction = (value - card.minimumValue) / range
                return height - clamp(fraction, 0, 1) * height
            }

            var intervalCount = card.stepValue > 0 ? Math.ceil(range / card.stepValue) : 1
            var referenceLineCount = Math.min(12, Math.max(1, intervalCount))
            var referenceStride = range / referenceLineCount
            context.strokeStyle = "#334155"
            context.lineWidth = 1
            for (var referenceIndex = 0; referenceIndex < referenceLineCount; ++referenceIndex) {
                var referenceY = yFor(card.minimumValue + referenceIndex * referenceStride)
                context.beginPath()
                context.moveTo(0, referenceY)
                context.lineTo(width, referenceY)
                context.stroke()
            }

            context.strokeStyle = "#38bdf8"
            context.lineWidth = 2
            var pathOpen = false
            for (var pointIndex = 0; pointIndex < card.points.length; ++pointIndex) {
                var point = card.points[pointIndex]
                var x = clamp(xFor(point), 0, width)
                var y = yFor(point.value)
                if (point.startsSegment || !pathOpen) {
                    if (pathOpen)
                        context.stroke()
                    context.beginPath()
                    context.moveTo(x, y)
                    pathOpen = true
                } else {
                    context.lineTo(x, y)
                }
            }
            if (pathOpen)
                context.stroke()

            context.fillStyle = "#fbbf24"
            for (var clippedIndex = 0; clippedIndex < card.points.length; ++clippedIndex) {
                var clippedPoint = card.points[clippedIndex]
                if (clippedPoint.value > card.maximumValue || clippedPoint.value < card.minimumValue) {
                    var clippedX = clamp(xFor(clippedPoint), 4, width - 4)
                    var edgeY = clippedPoint.value > card.maximumValue ? 0 : height
                    var direction = clippedPoint.value > card.maximumValue ? 1 : -1
                    context.beginPath()
                    context.moveTo(clippedX, edgeY)
                    context.lineTo(clippedX - 4, edgeY + direction * 6)
                    context.lineTo(clippedX + 4, edgeY + direction * 6)
                    context.closePath()
                    context.fill()
                }
            }
        }
    }

    Connections {
        target: card

        function onPointsChanged() { sparklineCanvas.requestPaint() }
        function onMinimumValueChanged() { sparklineCanvas.requestPaint() }
        function onMaximumValueChanged() { sparklineCanvas.requestPaint() }
        function onStepValueChanged() { sparklineCanvas.requestPaint() }
        function onHistorySecondsChanged() { sparklineCanvas.requestPaint() }
        function onCardReadingStateChanged() { sparklineCanvas.requestPaint() }
    }
}
