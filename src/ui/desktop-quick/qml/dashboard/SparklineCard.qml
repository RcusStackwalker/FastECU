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
    readonly property bool hasFiniteRange: Number.isFinite(minimumValue)
        && Number.isFinite(maximumValue) && maximumValue > minimumValue
        && Number.isFinite(maximumValue - minimumValue)
    readonly property real range: hasFiniteRange ? maximumValue - minimumValue : 0
    readonly property real referenceIntervalCount: hasFiniteRange && Number.isFinite(stepValue) && stepValue > 0
        ? range / stepValue : 0
    readonly property int referenceLineCount: Number.isFinite(referenceIntervalCount) && referenceIntervalCount > 0
        ? Math.min(12, Math.max(1, Math.ceil(referenceIntervalCount))) : 0
    readonly property real referenceGuideStrideMultiplier: {
        if (referenceLineCount === 0)
            return 0
        var stride = Math.max(1, Math.ceil(referenceIntervalCount / referenceLineCount))
        return Number.isSafeInteger(stride) ? stride : 0
    }
    readonly property real referenceGuideStepValue: {
        var renderedStep = stepValue * referenceGuideStrideMultiplier
        return Number.isFinite(renderedStep) && renderedStep > 0 ? renderedStep : 0
    }
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

            if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0
                    || !card.hasFiniteRange || !Number.isFinite(card.historySeconds)
                    || card.historySeconds <= 0 || card.points.length < 2)
                return

            var historyMilliseconds = card.historySeconds * 1000
            if (!Number.isFinite(historyMilliseconds) || historyMilliseconds <= 0)
                return
            var xFor = function(point) {
                var elapsed = Number(point.elapsedMs)
                var fraction = (elapsed + historyMilliseconds) / historyMilliseconds
                return Number.isFinite(fraction) ? fraction * width : NaN
            }
            var yFor = function(value) {
                var numericValue = Number(value)
                var fraction = (numericValue - card.minimumValue) / card.range
                return Number.isFinite(fraction) ? height - clamp(fraction, 0, 1) * height : NaN
            }

            if (card.referenceGuideStepValue > 0) {
                context.strokeStyle = "#334155"
                context.lineWidth = 1
                for (var referenceIndex = 0; referenceIndex < card.referenceLineCount; ++referenceIndex) {
                    var referenceValue = card.minimumValue + referenceIndex * card.referenceGuideStepValue
                    if (!Number.isFinite(referenceValue) || referenceValue > card.maximumValue)
                        break
                    var referenceY = yFor(referenceValue)
                    if (!Number.isFinite(referenceY))
                        continue
                    context.beginPath()
                    context.moveTo(0, referenceY)
                    context.lineTo(width, referenceY)
                    context.stroke()
                }
            }

            context.strokeStyle = "#38bdf8"
            context.lineWidth = 2
            var pathOpen = false
            for (var pointIndex = 0; pointIndex < card.points.length; ++pointIndex) {
                var point = card.points[pointIndex]
                var x = clamp(xFor(point), 0, width)
                var y = yFor(point.value)
                if (!Number.isFinite(x) || !Number.isFinite(y)) {
                    if (pathOpen)
                        context.stroke()
                    pathOpen = false
                    continue
                }
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
                var clippedValue = Number(clippedPoint.value)
                var clippedX = clamp(xFor(clippedPoint), 4, width - 4)
                if (Number.isFinite(clippedValue) && Number.isFinite(clippedX)
                        && (clippedValue > card.maximumValue || clippedValue < card.minimumValue)) {
                    var edgeY = clippedValue > card.maximumValue ? 0 : height
                    var direction = clippedValue > card.maximumValue ? 1 : -1
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
