// LiveChart.qml — canvas-based scrolling multi-series chart
// Supports a primary (left) Y axis and an optional secondary (right) Y axis.
import QtQuick
import QtQuick.Controls.Material

Item {
    id: root

    // ── Configuration ──────────────────────────────────────────────────────
    property string title:   ""
    property string leftUnit:  ""
    property string rightUnit: ""

    property int  windowSecs: 60
    property bool followMode: true

    // ── Range selection ────────────────────────────────────────────────────
    property real selectionStart: -1   // time in seconds, -1 = none
    property real selectionEnd:   -1
    signal rangeSelected(real tStart, real tEnd)

    // Exposed so parent can do pixel↔time mapping (updated each paint)
    property real _visXStart: 0
    property real _visXEnd:   windowSecs

    function pixelToTime(px) {
        var w = width - mL - mR
        if (w <= 0) return 0
        var frac = Math.max(0, Math.min(1, (px - mL) / w))
        return _visXStart + frac * (_visXEnd - _visXStart)
    }

    // ── Internal series state ──────────────────────────────────────────────
    property var seriesList: []
    property real leftYMin:  0.0
    property real leftYMax:  0.5
    property real rightYMin: 0.0
    property real rightYMax: 0.005
    property real xHead: 0.0

    // ── Public API ─────────────────────────────────────────────────────────
    function appendTo(idx, x, y) {
        if (idx < 0 || idx >= seriesList.length) return
        var s = seriesList[idx]
        s.data.push({ x: x, y: y })
        if (s.data.length > 3000)
            s.data.splice(0, s.data.length - 3000)
        if (x > xHead) xHead = x
        if (s.yAxis === "left") {
            if (y > leftYMax)  leftYMax  = y * 1.30 + 0.001
        } else {
            if (y > rightYMax) rightYMax = y * 1.30 + 0.0001
        }
        canvas.requestPaint()
    }

    function clearAll() {
        for (var i = 0; i < seriesList.length; i++)
            seriesList[i].data = []
        leftYMax  = 0.5
        rightYMax = 0.005
        xHead     = 0.0
        selectionStart = -1
        selectionEnd   = -1
        canvas.requestPaint()
    }

    // ── Layout margins ─────────────────────────────────────────────────────
    readonly property int mL: 58
    readonly property int mR: rightUnit !== "" ? 58 : 12
    readonly property int mT: 28
    readonly property int mB: 34

    // ── Canvas ─────────────────────────────────────────────────────────────
    Canvas {
        id: canvas
        anchors.fill: parent
        renderTarget: Canvas.Image

        onPaint: {
            var ctx = getContext("2d")
            var W = width, H = height
            ctx.clearRect(0, 0, W, H)

            var pX = root.mL, pY = root.mT
            var pW = W - root.mL - root.mR
            var pH = H - root.mT - root.mB
            if (pW < 10 || pH < 10) return

            var xEnd   = root.followMode
                            ? (root.xHead > 0 ? root.xHead + 0.5 : root.windowSecs)
                            : root.windowSecs
            var xStart = Math.max(0, xEnd - root.windowSecs)

            root._visXStart = xStart
            root._visXEnd   = xEnd

            // ── Background ───────────────────────────────────────────────
            ctx.fillStyle = "#16213e"
            ctx.fillRect(pX, pY, pW, pH)

            // ── Grid ──────────────────────────────────────────────────────
            var nGX = 6, nGY = 5
            ctx.strokeStyle = "#1f3060"
            ctx.lineWidth = 1
            for (var i = 0; i <= nGX; i++) {
                var gx = pX + i * pW / nGX
                ctx.beginPath(); ctx.moveTo(gx, pY); ctx.lineTo(gx, pY + pH); ctx.stroke()
            }
            for (var j = 0; j <= nGY; j++) {
                var gy = pY + j * pH / nGY
                ctx.beginPath(); ctx.moveTo(pX, gy); ctx.lineTo(pX + pW, gy); ctx.stroke()
            }

            // ── Mappers ───────────────────────────────────────────────────
            var lYMin = root.leftYMin,  lYMax = root.leftYMax
            var rYMin = root.rightYMin, rYMax = root.rightYMax
            function mx(x)  { return pX + (x - xStart) / (xEnd - xStart) * pW }
            function myL(y) { return pY + pH - (y - lYMin) / (lYMax - lYMin) * pH }
            function myR(y) { return pY + pH - (y - rYMin) / (rYMax - rYMin) * pH }

            // ── Clip to plot area ─────────────────────────────────────────
            ctx.save()
            ctx.beginPath()
            ctx.rect(pX, pY, pW, pH)
            ctx.clip()

            // ── Selection highlight ───────────────────────────────────────
            var sSt = root.selectionStart, sEn = root.selectionEnd
            if (sSt >= 0 && sEn >= 0) {
                var sxA = Math.max(pX, mx(Math.min(sSt, sEn)))
                var sxB = Math.min(pX + pW, mx(Math.max(sSt, sEn)))
                if (sxB > sxA) {
                    ctx.fillStyle = "rgba(100,160,255,0.12)"
                    ctx.fillRect(sxA, pY, sxB - sxA, pH)
                    ctx.strokeStyle = "rgba(100,160,255,0.6)"
                    ctx.lineWidth = 1
                    ctx.setLineDash([4, 3])
                    ctx.beginPath(); ctx.moveTo(sxA, pY); ctx.lineTo(sxA, pY + pH); ctx.stroke()
                    ctx.beginPath(); ctx.moveTo(sxB, pY); ctx.lineTo(sxB, pY + pH); ctx.stroke()
                    ctx.setLineDash([])
                }
            }

            // ── Draw series ───────────────────────────────────────────────
            for (var si = 0; si < root.seriesList.length; si++) {
                var s    = root.seriesList[si]
                var data = s.data
                if (data.length < 2) continue
                var mapY = (s.yAxis === "right") ? myR : myL
                var margin = (xEnd - xStart) / pW

                if (s.fillArea) {
                    ctx.beginPath()
                    var firstA = true
                    var lastPx = 0
                    for (var k = 0; k < data.length; k++) {
                        if (data[k].x < xStart - margin) continue
                        var px = mx(data[k].x), py = mapY(data[k].y)
                        if (firstA) { ctx.moveTo(px, mapY(0)); ctx.lineTo(px, py); firstA = false }
                        else        { ctx.lineTo(px, py) }
                        lastPx = px
                    }
                    if (!firstA) {
                        ctx.lineTo(lastPx, mapY(0))
                        ctx.closePath()
                        ctx.fillStyle = s.fillColor || Qt.rgba(1,1,1,0.08)
                        ctx.fill()
                    }
                }

                ctx.beginPath()
                var firstL = true
                for (var k = 0; k < data.length; k++) {
                    if (data[k].x < xStart - margin) continue
                    var px = mx(data[k].x), py = mapY(data[k].y)
                    if (firstL) { ctx.moveTo(px, py); firstL = false }
                    else        { ctx.lineTo(px, py) }
                }
                ctx.strokeStyle = s.color
                ctx.lineWidth = s.yAxis === "right" ? 1.5 : 2
                ctx.setLineDash(s.yAxis === "right" ? [4, 2] : [])
                ctx.stroke()
                ctx.setLineDash([])
            }

            ctx.restore()

            // ── Border ────────────────────────────────────────────────────
            ctx.strokeStyle = "#334"
            ctx.lineWidth = 1
            ctx.strokeRect(pX, pY, pW, pH)

            // ── X axis labels ─────────────────────────────────────────────
            ctx.fillStyle = "#aab"
            ctx.font = "11px monospace"
            ctx.textAlign = "center"
            ctx.textBaseline = "top"
            for (var i = 0; i <= nGX; i++) {
                var gx = pX + i * pW / nGX
                var xv = xStart + i * (xEnd - xStart) / nGX
                ctx.fillText(xv.toFixed(0), gx, pY + pH + 4)
            }
            ctx.fillText("Time (s)", pX + pW / 2, pY + pH + 18)

            // ── Selection time labels ─────────────────────────────────────
            if (sSt >= 0 && sEn >= 0) {
                var t0 = Math.min(sSt, sEn), t1 = Math.max(sSt, sEn)
                ctx.fillStyle = "rgba(150,200,255,0.85)"
                ctx.font = "10px monospace"
                ctx.textAlign = "center"
                ctx.textBaseline = "bottom"
                var sxA2 = mx(t0), sxB2 = mx(t1)
                if (sxA2 >= pX && sxA2 <= pX + pW)
                    ctx.fillText(t0.toFixed(1)+"s", sxA2, pY - 2)
                if (sxB2 >= pX && sxB2 <= pX + pW)
                    ctx.fillText(t1.toFixed(1)+"s", sxB2, pY - 2)
            }

            // ── Left Y labels ─────────────────────────────────────────────
            ctx.fillStyle = "#aab"
            ctx.font = "11px monospace"
            ctx.textAlign = "right"
            ctx.textBaseline = "middle"
            for (var j = 0; j <= nGY; j++) {
                var gy  = pY + (nGY - j) * pH / nGY
                var yv  = lYMin + j * (lYMax - lYMin) / nGY
                var lbl = yv < 1 ? yv.toFixed(3) : yv < 10 ? yv.toFixed(2) : yv.toFixed(1)
                ctx.fillText(lbl, pX - 4, gy)
            }
            if (root.leftUnit !== "") {
                ctx.save()
                ctx.translate(11, pY + pH / 2)
                ctx.rotate(-Math.PI / 2)
                ctx.textAlign = "center"
                ctx.font = "11px sans-serif"
                ctx.fillStyle = "#ccd"
                ctx.fillText(root.leftUnit, 0, 0)
                ctx.restore()
            }

            // ── Right Y labels ────────────────────────────────────────────
            if (root.rightUnit !== "") {
                ctx.textAlign = "left"
                ctx.textBaseline = "middle"
                for (var j = 0; j <= nGY; j++) {
                    var gy  = pY + (nGY - j) * pH / nGY
                    var yv  = rYMin + j * (rYMax - rYMin) / nGY
                    var lbl = yv < 1 ? yv.toFixed(3) : yv < 10 ? yv.toFixed(2) : yv.toFixed(1)
                    ctx.fillStyle = "#aab"
                    ctx.fillText(lbl, pX + pW + 4, gy)
                }
                ctx.save()
                ctx.translate(W - 11, pY + pH / 2)
                ctx.rotate(Math.PI / 2)
                ctx.textAlign = "center"
                ctx.font = "11px sans-serif"
                ctx.fillStyle = "#ccd"
                ctx.fillText(root.rightUnit, 0, 0)
                ctx.restore()
            }

            // ── Title ─────────────────────────────────────────────────────
            ctx.fillStyle = "#dde"
            ctx.font = "bold 12px sans-serif"
            ctx.textAlign = "left"
            ctx.textBaseline = "top"
            ctx.fillText(root.title, pX + 4, 6)

            // ── Legend ────────────────────────────────────────────────────
            var legX = pX + pW / 2 - 20
            var legY = 8
            ctx.textBaseline = "middle"
            ctx.font = "11px sans-serif"
            for (var si = 0; si < root.seriesList.length; si++) {
                var ser = root.seriesList[si]
                ctx.fillStyle = ser.color
                ctx.fillRect(legX, legY - 1, 14, 2)
                ctx.fillText(ser.name, legX + 18, legY)
                legX += ctx.measureText(ser.name).width + 38
            }
        }
    }

    // ── Mouse interaction for range selection ──────────────────────────────
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.CrossCursor
        property real dragStartTime: -1

        onPressed: (mouse) => {
            dragStartTime = root.pixelToTime(mouse.x)
            root.selectionStart = dragStartTime
            root.selectionEnd   = dragStartTime
            canvas.requestPaint()
        }
        onPositionChanged: (mouse) => {
            if (pressed) {
                root.selectionEnd = root.pixelToTime(mouse.x)
                canvas.requestPaint()
            }
        }
        onReleased: (mouse) => {
            root.selectionEnd = root.pixelToTime(mouse.x)
            var t0 = Math.min(root.selectionStart, root.selectionEnd)
            var t1 = Math.max(root.selectionStart, root.selectionEnd)
            if (t1 - t0 > 0.05)
                root.rangeSelected(t0, t1)
            canvas.requestPaint()
        }
        onDoubleClicked: {
            root.selectionStart = -1
            root.selectionEnd   = -1
            canvas.requestPaint()
        }
    }
}
