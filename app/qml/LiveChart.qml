// LiveChart.qml — canvas-based scrolling multi-series chart
import QtQuick
import QtQuick.Controls.Material

Item {
    id: root

    property string title:     ""
    property string leftUnit:  ""
    property string rightUnit: ""

    // Controlled from Main.qml (shared across both charts)
    property real effectiveWindowSecs: 60
    property real viewLeft:  0
    property bool followMode: true

    property real selectionStart: -1
    property real selectionEnd:   -1
    signal rangeSelected(real tStart, real tEnd)

    // Zoom/pan: parent syncs both charts
    signal viewChanged(real newViewLeft, real newWindowSecs)

    property var  seriesList: []
    property real leftYMin:  0.0
    property real leftYMax:  0.5
    property real rightYMin: 0.0
    property real rightYMax: 0.005
    property real xHead:     0.0

    function appendTo(idx, x, y) {
        if (idx < 0 || idx >= seriesList.length) return
        var s = seriesList[idx]
        s.data.push({ x: x, y: y })
        if (s.data.length > 6000) s.data.splice(0, s.data.length - 6000)
        if (x > xHead) xHead = x
        if (s.yAxis === "left") { if (y > leftYMax)  leftYMax  = y * 1.30 + 0.001 }
        else                    { if (y > rightYMax) rightYMax = y * 1.30 + 0.0001 }
        canvas.requestPaint()
    }

    function clearAll() {
        for (var i = 0; i < seriesList.length; i++) seriesList[i].data = []
        leftYMax = 0.5; rightYMax = 0.005; xHead = 0.0
        selectionStart = -1; selectionEnd = -1
        canvas.requestPaint()
    }

    function currentViewLeft() {
        if (followMode) return Math.max(0, xHead - effectiveWindowSecs + 0.5)
        return Math.max(0, viewLeft)
    }

    function pixelToTime(px) {
        var w = width - mL - mR
        if (w <= 0) return 0
        return currentViewLeft() + Math.max(0, Math.min(1, (px - mL) / w)) * effectiveWindowSecs
    }

    readonly property int mL: 58
    readonly property int mR: rightUnit !== "" ? 58 : 12
    readonly property int mT: 28
    readonly property int mB: 34

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

            var xStart = root.currentViewLeft()
            var xEnd   = xStart + root.effectiveWindowSecs

            ctx.fillStyle = "#16213e"; ctx.fillRect(pX, pY, pW, pH)

            var nGX = 6, nGY = 5
            ctx.strokeStyle = "#1f3060"; ctx.lineWidth = 1
            for (var i = 0; i <= nGX; i++) {
                var gx = pX + i * pW / nGX
                ctx.beginPath(); ctx.moveTo(gx,pY); ctx.lineTo(gx,pY+pH); ctx.stroke()
            }
            for (var j = 0; j <= nGY; j++) {
                var gy = pY + j * pH / nGY
                ctx.beginPath(); ctx.moveTo(pX,gy); ctx.lineTo(pX+pW,gy); ctx.stroke()
            }

            var lYMin=root.leftYMin, lYMax=root.leftYMax
            var rYMin=root.rightYMin, rYMax=root.rightYMax
            function mx(x)  { return pX + (x-xStart)/(xEnd-xStart)*pW }
            function myL(y) { return pY + pH - (y-lYMin)/(lYMax-lYMin)*pH }
            function myR(y) { return pY + pH - (y-rYMin)/(rYMax-rYMin)*pH }

            ctx.save(); ctx.beginPath(); ctx.rect(pX,pY,pW,pH); ctx.clip()

            // Selection highlight
            var sSt=root.selectionStart, sEn=root.selectionEnd
            if (sSt>=0 && sEn>=0) {
                var sxA=mx(Math.min(sSt,sEn)), sxB=mx(Math.max(sSt,sEn))
                if (sxB>sxA) {
                    ctx.fillStyle="rgba(100,160,255,0.12)"; ctx.fillRect(sxA,pY,sxB-sxA,pH)
                    ctx.strokeStyle="rgba(100,160,255,0.6)"; ctx.lineWidth=1; ctx.setLineDash([4,3])
                    ctx.beginPath(); ctx.moveTo(sxA,pY); ctx.lineTo(sxA,pY+pH); ctx.stroke()
                    ctx.beginPath(); ctx.moveTo(sxB,pY); ctx.lineTo(sxB,pY+pH); ctx.stroke()
                    ctx.setLineDash([])
                }
            }

            // Series
            for (var si=0; si<root.seriesList.length; si++) {
                var s=root.seriesList[si], data=s.data
                if (data.length<2) continue
                var mapY=(s.yAxis==="right")?myR:myL, margin=(xEnd-xStart)/pW
                if (s.fillArea) {
                    ctx.beginPath()
                    var fa=true, lpx2=0
                    for (var k=0; k<data.length; k++) {
                        if (data[k].x<xStart-margin||data[k].x>xEnd+margin) continue
                        var fpx=mx(data[k].x), fpy=mapY(data[k].y)
                        if (fa){ctx.moveTo(fpx,mapY(0));ctx.lineTo(fpx,fpy);fa=false}
                        else ctx.lineTo(fpx,fpy); lpx2=fpx
                    }
                    if (!fa){ctx.lineTo(lpx2,mapY(0));ctx.closePath()
                        ctx.fillStyle=s.fillColor||Qt.rgba(1,1,1,0.08);ctx.fill()}
                }
                ctx.beginPath(); var fl=true
                for (var k=0; k<data.length; k++) {
                    if (data[k].x<xStart-margin||data[k].x>xEnd+margin) continue
                    var lpx=mx(data[k].x), lpy=mapY(data[k].y)
                    if(fl){ctx.moveTo(lpx,lpy);fl=false}else ctx.lineTo(lpx,lpy)
                }
                ctx.strokeStyle=s.color; ctx.lineWidth=s.yAxis==="right"?1.5:2
                ctx.setLineDash(s.yAxis==="right"?[4,2]:[])
                ctx.stroke(); ctx.setLineDash([])
            }
            ctx.restore()

            ctx.strokeStyle="#334"; ctx.lineWidth=1; ctx.strokeRect(pX,pY,pW,pH)

            // X axis
            ctx.fillStyle="#aab"; ctx.font="11px monospace"
            ctx.textAlign="center"; ctx.textBaseline="top"
            for (var i=0;i<=nGX;i++) {
                var gx2=pX+i*pW/nGX, xv=xStart+i*(xEnd-xStart)/nGX
                ctx.fillText(xv.toFixed(0),gx2,pY+pH+4)
            }
            ctx.fillText("Time (s)",pX+pW/2,pY+pH+18)

            // Selection time labels
            if (sSt>=0&&sEn>=0) {
                ctx.fillStyle="rgba(150,200,255,0.85)"; ctx.font="10px monospace"
                ctx.textBaseline="bottom"; ctx.textAlign="center"
                var t0l=Math.min(sSt,sEn), t1l=Math.max(sSt,sEn)
                var sxA2=mx(t0l), sxB2=mx(t1l)
                if(sxA2>=pX&&sxA2<=pX+pW) ctx.fillText(t0l.toFixed(1)+"s",sxA2,pY-2)
                if(sxB2>=pX&&sxB2<=pX+pW) ctx.fillText(t1l.toFixed(1)+"s",sxB2,pY-2)
            }

            // Left Y
            ctx.fillStyle="#aab"; ctx.font="11px monospace"
            ctx.textAlign="right"; ctx.textBaseline="middle"
            for (var j2=0;j2<=nGY;j2++) {
                var gy2=pY+(nGY-j2)*pH/nGY, yv=lYMin+j2*(lYMax-lYMin)/nGY
                ctx.fillText(yv<1?yv.toFixed(3):yv<10?yv.toFixed(2):yv.toFixed(1),pX-4,gy2)
            }
            if (root.leftUnit!=="") {
                ctx.save();ctx.translate(11,pY+pH/2);ctx.rotate(-Math.PI/2)
                ctx.textAlign="center";ctx.font="11px sans-serif";ctx.fillStyle="#ccd"
                ctx.fillText(root.leftUnit,0,0);ctx.restore()
            }

            // Right Y
            if (root.rightUnit!=="") {
                ctx.textAlign="left";ctx.textBaseline="middle"
                for (var j3=0;j3<=nGY;j3++) {
                    var gy3=pY+(nGY-j3)*pH/nGY, rv=rYMin+j3*(rYMax-rYMin)/nGY
                    ctx.fillStyle="#aab"
                    ctx.fillText(rv<1?rv.toFixed(3):rv<10?rv.toFixed(2):rv.toFixed(1),pX+pW+4,gy3)
                }
                ctx.save();ctx.translate(W-11,pY+pH/2);ctx.rotate(Math.PI/2)
                ctx.textAlign="center";ctx.font="11px sans-serif";ctx.fillStyle="#ccd"
                ctx.fillText(root.rightUnit,0,0);ctx.restore()
            }

            // Title + legend
            ctx.fillStyle="#dde";ctx.font="bold 12px sans-serif"
            ctx.textAlign="left";ctx.textBaseline="top"
            ctx.fillText(root.title,pX+4,6)
            var legX=pX+pW/2-20,legY=8
            ctx.textBaseline="middle";ctx.font="11px sans-serif"
            for (var si2=0;si2<root.seriesList.length;si2++) {
                var ser=root.seriesList[si2]
                ctx.fillStyle=ser.color;ctx.fillRect(legX,legY-1,14,2)
                ctx.fillText(ser.name,legX+18,legY)
                legX+=ctx.measureText(ser.name).width+38
            }

            // Hint
            ctx.fillStyle="rgba(170,170,187,0.28)";ctx.font="10px sans-serif"
            ctx.textAlign="right";ctx.textBaseline="bottom"
            ctx.fillText("scroll=zoom  RMB=pan  LMB=measure",pX+pW-4,pY+pH-4)
        }
    }

    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (event) => {
            var factor = event.angleDelta.y > 0 ? 0.75 : 1.33
            var ws = root.effectiveWindowSecs
            var vl = root.currentViewLeft()
            var frac = Math.max(0, Math.min(1,
                (point.position.x - root.mL) / Math.max(1, root.width - root.mL - root.mR)))
            var mouseT = vl + frac * ws
            var newWs = Math.max(2, Math.min(7200, ws * factor))
            var newVl = Math.max(0, mouseT - frac * newWs)
            root.viewChanged(newVl, newWs)
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: pressedButtons & Qt.RightButton ? Qt.ClosedHandCursor : Qt.CrossCursor
        property real _panStartX:  0
        property real _panStartVL: 0

        onPressed: (mouse) => {
            if (mouse.button === Qt.RightButton) {
                _panStartX  = mouse.x
                _panStartVL = root.currentViewLeft()
            } else {
                root.selectionStart = root.pixelToTime(mouse.x)
                root.selectionEnd   = root.selectionStart
                canvas.requestPaint()
            }
        }
        onPositionChanged: (mouse) => {
            if (pressedButtons & Qt.RightButton) {
                var dt = -(mouse.x - _panStartX) / Math.max(1, root.width - root.mL - root.mR) * root.effectiveWindowSecs
                root.viewChanged(Math.max(0, _panStartVL + dt), root.effectiveWindowSecs)
            } else if (pressed) {
                root.selectionEnd = root.pixelToTime(mouse.x)
                canvas.requestPaint()
            }
        }
        onReleased: (mouse) => {
            if (mouse.button === Qt.LeftButton) {
                root.selectionEnd = root.pixelToTime(mouse.x)
                var t0 = Math.min(root.selectionStart, root.selectionEnd)
                var t1 = Math.max(root.selectionStart, root.selectionEnd)
                if (t1 - t0 > 0.05) root.rangeSelected(t0, t1)
                canvas.requestPaint()
            }
        }
        onDoubleClicked: {
            root.selectionStart = -1; root.selectionEnd = -1
            canvas.requestPaint()
        }
    }
}
