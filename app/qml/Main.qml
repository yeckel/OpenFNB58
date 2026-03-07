// main.qml — FNB58 monitor main window
import QtQuick
import QtQuick.Controls.Material
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Qt.labs.platform as Platform

ApplicationWindow {
    id: window
    visible: true
    width:  1200
    height: 750
    title:  "OpenFNB58"
    color:  "#0d1117"

    Material.theme:   Material.Dark
    Material.accent:  Material.LightBlue
    Material.primary: "#1a1f2e"

    // ── Toolbar ───────────────────────────────────────────────────────────
    header: ToolBar {
        height: 48
        background: Rectangle { color: "#161b22" }

        RowLayout {
            anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
            spacing: 6

            // Start / Stop
            Button {
                id: startStopBtn
                text: backend.running ? "■  Stop" : "▶  Start"
                highlighted: !backend.running
                Material.accent: backend.running ? Material.Red : Material.LightBlue
                onClicked: backend.running ? backend.stop() : backend.start(
                    modeCombo.currentIndex === 0 ? "usb" : "ble",
                    deviceEdit.text.trim())
            }

            ToolSeparator {}

            // Transport selector
            Label { text: "Mode:"; opacity: 0.7 }
            ComboBox {
                id: modeCombo
                model: ["USB", "BLE"]
                width: 90
                enabled: !backend.running
            }

            // Device / MAC field
            Label { text: "Device / MAC:"; opacity: 0.7; visible: modeCombo.currentIndex === 1 }
            TextField {
                id: deviceEdit
                placeholderText: modeCombo.currentIndex === 0 ? "(auto)" : "XX:XX:XX:XX:XX:XX"
                width: 180
                enabled: !backend.running
                color: Material.foreground
                visible: modeCombo.currentIndex === 1
            }
            // BLE scan button
            Button {
                text: "⊕  Scan"
                visible: modeCombo.currentIndex === 1 && !backend.running
                onClicked: {
                    backend.scanBleDevices()
                    bleScanPopup.open()
                }
            }

            ToolSeparator {}

            // Window selector
            Label { text: "Window:"; opacity: 0.7 }
            ComboBox {
                id: windowCombo
                model: ["30 s", "60 s", "120 s", "300 s", "600 s"]
                width: 90
                property var secs: [30, 60, 120, 300, 600]
                currentIndex: 1
                onCurrentIndexChanged: {
                    voltChart.windowSecs  = secs[currentIndex]
                    powerChart.windowSecs = secs[currentIndex]
                }
            }

            // Follow toggle
            ToolButton {
                id: followBtn
                text: "⬤  Follow"
                checkable: true
                checked: true
                ToolTip.text: "Scroll to follow latest data"
                ToolTip.visible: hovered
                onCheckedChanged: {
                    voltChart.followMode  = checked
                    powerChart.followMode = checked
                }
            }

            ToolSeparator {}

            // Energy reset marker
            Button {
                text: "⌀  Mark Energy"
                onClicked: backend.resetEnergy()
                ToolTip.text: "Reset energy accumulator"
                ToolTip.visible: hovered
            }

            Item { Layout.fillWidth: true }

            // CSV export
            Button {
                text: "Export CSV"
                enabled: backend.sampleCount > 0
                onClicked: csvDialog.open()
            }

            // XLSX export
            Button {
                text: "Export XLSX"
                enabled: backend.sampleCount > 0
                onClicked: xlsxDialog.open()
            }
        }
    }

    // ── File Dialogs (native via Qt.labs.platform + QApplication) ────────
    Platform.FileDialog {
        id: csvDialog
        title:         "Export CSV"
        fileMode:      Platform.FileDialog.SaveFile
        nameFilters:   ["CSV files (*.csv)", "All files (*)"]
        defaultSuffix: "csv"
        onAccepted: backend.exportCsv(file)
    }

    Platform.FileDialog {
        id: xlsxDialog
        title:         "Export Excel"
        fileMode:      Platform.FileDialog.SaveFile
        nameFilters:   ["Excel files (*.xlsx)", "All files (*)"]
        defaultSuffix: "xlsx"
        onAccepted: backend.exportExcel(file)
    }

    // ── BLE device picker popup ───────────────────────────────────────────
    Popup {
        id: bleScanPopup
        x: (parent.width  - width)  / 2
        y: (parent.height - height) / 2
        width: 380; height: Math.min(300, bleDeviceList.count * 48 + 80)
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: "#1a1f2e"; radius: 8; border { color: "#334"; width: 1 } }

        ColumnLayout {
            anchors { fill: parent; margins: 12 }
            spacing: 8
            Label {
                text: "Select BLE Device"
                font { pixelSize: 14; bold: true }
                color: "#ccd"
            }
            Label {
                id: bleScanStatus
                text: "Scanning…"
                font.pixelSize: 12; opacity: 0.7
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            ListView {
                id: bleDeviceList
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: []
                clip: true
                delegate: ItemDelegate {
                    required property string modelData
                    width: ListView.view.width
                    text: modelData
                    onClicked: {
                        // Extract MAC from "Name (MAC)"
                        var m = modelData.match(/\(([0-9A-Fa-f:]{17})\)/)
                        if (m) deviceEdit.text = m[1]
                        bleScanPopup.close()
                    }
                }
            }
            Button {
                text: "Close"
                Layout.alignment: Qt.AlignRight
                onClicked: bleScanPopup.close()
            }
        }
    }

    Connections {
        target: backend
        function onBleDevicesFound(devices) {
            bleDeviceList.model = devices
            bleScanStatus.text  = devices.length > 0
                ? "Tap a device to select it"
                : "No paired devices found. Pair with bluetoothctl first."
        }
    }

    // ── Status bar ────────────────────────────────────────────────────────
    footer: Rectangle {
        height: 28
        color: "#161b22"
        RowLayout {
            anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
            Label {
                id: statusLabel
                text: "Ready"
                font.pixelSize: 12
                opacity: 0.85
            }
            Item { Layout.fillWidth: true }
            Label {
                text: "Duration: " + backend.duration
                font.pixelSize: 12
                opacity: 0.7
            }
            Label {
                text: "Samples: " + backend.sampleCount
                font.pixelSize: 12
                opacity: 0.7
            }
            Label {
                text: "Energy: " + backend.energyWh.toFixed(4) + " Wh"
                font.pixelSize: 12
                color: Material.accentColor
            }
        }
    }

    // ── Shared measurement range state ────────────────────────────────────
    QtObject {
        id: measurement
        property real  tStart:     -1
        property real  tEnd:       -1
        property var   stats:      null   // QVariantMap from backend.measureRange()
        property bool  hasRange:   tStart >= 0 && tEnd > tStart

        function update(t0, t1) {
            tStart = t0; tEnd = t1
            stats  = backend.measureRange(t0, t1)
            // sync both charts
            voltChart.selectionStart  = t0; voltChart.selectionEnd  = t1
            powerChart.selectionStart = t0; powerChart.selectionEnd = t1
        }
        function clear() {
            tStart = -1; tEnd = -1; stats = null
            voltChart.selectionStart  = -1; voltChart.selectionEnd  = -1
            powerChart.selectionStart = -1; powerChart.selectionEnd = -1
        }
    }

    // ── Readings panel (top) ──────────────────────────────────────────────
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Readout bar
        Rectangle {
            Layout.fillWidth: true
            height: 70
            color: "#161b22"
            RowLayout {
                anchors { fill: parent; leftMargin: 16; rightMargin: 16 }
                spacing: 24

                Repeater {
                    model: [
                        { label: "VBUS",  value: live.vbus,  fmt: function(v){ return v.toFixed(3)+" V" },  clr: "#58a6ff" },
                        { label: "IBUS",  value: live.ibus,  fmt: function(v){ return v.toFixed(4)+" A" },  clr: "#d2a8ff" },
                        { label: "Power", value: live.power, fmt: function(v){ return v.toFixed(3)+" W" },  clr: "#ffa657" },
                        { label: "D+",    value: live.dp,    fmt: function(v){ return v.toFixed(3)+" V" },  clr: "#3fb950" },
                        { label: "D−",    value: live.dn,    fmt: function(v){ return v.toFixed(3)+" V" },  clr: "#f78166" },
                        { label: "Temp",  value: live.temp,  fmt: function(v){ return isNaN(v)?"—":v.toFixed(1)+" °C" }, clr: "#e3b341" }
                    ]
                    delegate: ColumnLayout {
                        required property var modelData
                        spacing: 2
                        Label {
                            text: modelData.label
                            font.pixelSize: 11
                            opacity: 0.6
                        }
                        Label {
                            text: modelData.fmt(modelData.value)
                            font.pixelSize: 26
                            font.bold: true
                            color: modelData.clr
                        }
                    }
                }
            }
        }

        // ── Chart area ────────────────────────────────────────────────────
        SplitView {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            orientation: Qt.Vertical

            // Voltage + Current (dual axis)
            LiveChart {
                id: voltChart
                SplitView.fillWidth:      true
                SplitView.preferredHeight: parent.height * 0.4
                title:     "Voltage / Current"
                leftUnit:  "Voltage (V)"
                rightUnit: "Current (A)"
                followMode: followBtn.checked
                seriesList: [
                    { name: "VBUS",  color: "#58a6ff", data: [], yAxis: "left",  fillArea: false },
                    { name: "D+",    color: "#3fb950", data: [], yAxis: "left",  fillArea: false },
                    { name: "D−",    color: "#f78166", data: [], yAxis: "left",  fillArea: false },
                    { name: "IBUS",  color: "#d2a8ff", data: [], yAxis: "right", fillArea: false }
                ]
                onRangeSelected: (t0, t1) => measurement.update(t0, t1)
            }

            // Power (single axis, area fill)
            LiveChart {
                id: powerChart
                SplitView.fillWidth:      true
                SplitView.fillHeight:     true
                title:    "Power"
                leftUnit: "Power (W)"
                followMode: followBtn.checked
                seriesList: [
                    { name: "Power", color: "#ffa657",
                      data: [], yAxis: "left", fillArea: true,
                      fillColor: Qt.rgba(1, 0.65, 0.34, 0.15) }
                ]
                onRangeSelected: (t0, t1) => measurement.update(t0, t1)
            }
        }

        // ── Measurement stats panel ───────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: measurement.hasRange ? 110 : 0
            Layout.maximumHeight:   measurement.hasRange ? 110 : 0
            clip: true
            color: "#0f1923"

            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
            }

            RowLayout {
                anchors { fill: parent; margins: 10 }
                spacing: 0

                // Title + clear button
                ColumnLayout {
                    spacing: 4
                    Layout.preferredWidth: 130
                    Label {
                        text: "📐 Measurement"
                        font { pixelSize: 12; bold: true }
                        color: "#7cb8ff"
                    }
                    Label {
                        text: measurement.hasRange
                            ? (measurement.tStart.toFixed(1)+"s → "+measurement.tEnd.toFixed(1)+"s")
                            : ""
                        font.pixelSize: 11; opacity: 0.6
                    }
                    Label {
                        text: measurement.stats
                            ? "Δt = " + (measurement.tEnd - measurement.tStart).toFixed(2) + " s  |  "
                              + (measurement.stats.sampleCount || 0) + " samples"
                            : ""
                        font.pixelSize: 11; opacity: 0.55
                    }
                    Button {
                        text: "✕ Clear"
                        font.pixelSize: 11
                        flat: true
                        padding: 4
                        onClicked: measurement.clear()
                    }
                }

                Rectangle { width: 1; height: parent.height - 16; color: "#234"; Layout.alignment: Qt.AlignVCenter }

                // Stats grid
                GridLayout {
                    columns: 6
                    columnSpacing: 24
                    rowSpacing: 4
                    Layout.leftMargin: 16

                    // Headers
                    Repeater {
                        model: ["Energy", "Mean V", "Peak V", "Mean I", "Peak I", "Mean P  /  Peak P"]
                        Label {
                            required property string modelData
                            text: modelData
                            font.pixelSize: 10
                            font.bold: true
                            opacity: 0.55
                            color: "#aab"
                        }
                    }

                    // Values
                    Label {
                        text: {
                            if (!measurement.stats) return "—"
                            var wh = measurement.stats.energyWh || 0
                            if (wh < 0.001) return (wh * 1e6).toFixed(1) + " μWh"
                            if (wh < 1)     return (wh * 1000).toFixed(3) + " mWh\n"
                                                  + (measurement.stats.energyMAh || 0).toFixed(2) + " mAh"
                            return wh.toFixed(4) + " Wh\n"
                                 + (measurement.stats.energyMAh || 0).toFixed(2) + " mAh"
                        }
                        font.pixelSize: 13
                        font.bold: true
                        color: "#ffa657"
                        lineHeight: 1.3
                    }
                    Label {
                        text: measurement.stats ? (measurement.stats.meanVbus||0).toFixed(4)+" V" : "—"
                        font.pixelSize: 13
                        font.bold: true
                        color: "#58a6ff"
                    }
                    Label {
                        text: measurement.stats ? (measurement.stats.peakVbus||0).toFixed(4)+" V" : "—"
                        font.pixelSize: 13
                        font.bold: true
                        color: "#58a6ff"
                        opacity: 0.8
                    }
                    Label {
                        text: measurement.stats ? (measurement.stats.meanIbus||0).toFixed(4)+" A" : "—"
                        font.pixelSize: 13
                        font.bold: true
                        color: "#d2a8ff"
                    }
                    Label {
                        text: measurement.stats ? (measurement.stats.peakIbus||0).toFixed(4)+" A" : "—"
                        font.pixelSize: 13
                        font.bold: true
                        color: "#d2a8ff"
                        opacity: 0.8
                    }
                    Label {
                        text: measurement.stats
                            ? (measurement.stats.meanPower||0).toFixed(3)+" W  /  "
                              + (measurement.stats.peakPower||0).toFixed(3)+" W"
                            : "—"
                        font.pixelSize: 13
                        font.bold: true
                        color: "#ffa657"
                        opacity: 0.9
                    }
                }

                Item { Layout.fillWidth: true }

                // Temp column (only if available)
                ColumnLayout {
                    spacing: 4
                    visible: measurement.stats && measurement.stats.minTemp !== undefined
                    Label {
                        text: "Temperature"
                        font.pixelSize: 10
                        font.bold: true
                        opacity: 0.55
                        color: "#aab"
                    }
                    Label {
                        text: measurement.stats && measurement.stats.minTemp !== undefined
                            ? (measurement.stats.minTemp).toFixed(1) + " °C  min\n"
                              + (measurement.stats.maxTemp).toFixed(1) + " °C  max"
                            : ""
                        font.pixelSize: 13
                        font.bold: true
                        color: "#e3b341"
                        lineHeight: 1.3
                    }
                }
            }
        }
    }

    // ── Readout model — live values updated by backend ────────────────────
    QtObject {
        id: live
        property real vbus:  0.0
        property real ibus:  0.0
        property real power: 0.0
        property real dp:    0.0
        property real dn:    0.0
        property real temp:  NaN
    }

    // ── Backend connections ───────────────────────────────────────────────
    Connections {
        target: backend

        function onNewReading(t, vbus, ibus, power, dp, dn, temp) {
            // Update live readouts
            live.vbus  = vbus
            live.ibus  = ibus
            live.power = power
            live.dp    = dp
            live.dn    = dn
            live.temp  = temp

            // Append to charts
            voltChart.appendTo(0, t, vbus)
            voltChart.appendTo(1, t, dp)
            voltChart.appendTo(2, t, dn)
            voltChart.appendTo(3, t, ibus)

            powerChart.appendTo(0, t, power)
        }

        function onStatusChanged(msg) {
            statusLabel.text  = msg
            statusLabel.color = Material.foreground
        }

        function onErrorOccurred(msg) {
            statusLabel.text  = "⚠ " + msg
            statusLabel.color = "#f78166"
        }

        function onRunningChanged() {
            if (backend.running) statusLabel.color = Material.foreground
        }
    }

    // ── Reset charts when starting ────────────────────────────────────────
    Connections {
        target: backend
        function onRunningChanged() {
            if (backend.running) {
                voltChart.clearAll()
                powerChart.clearAll()
                measurement.clear()
            }
        }
    }

    // ── Error toast ───────────────────────────────────────────────────────
    Popup {
        id: errorPopup
        property alias message: toastLabel.text
        x: (parent.width  - width)  / 2
        y:  parent.height - height - 60
        width:  Math.min(parent.width - 40, 500)
        height: 52
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: "#3d1717"
            radius: 6
            border { color: "#f78166"; width: 1 }
        }
        Label {
            id: toastLabel
            anchors.centerIn: parent
            color: "#f78166"
            font.pixelSize: 13
            wrapMode: Text.Wrap
            width: parent.width - 20
        }
        Timer { id: toastTimer; interval: 4000; onTriggered: errorPopup.close() }
    }

    Connections {
        target: backend
        function onErrorOccurred(msg) {
            errorPopup.message = msg
            errorPopup.open()
            toastTimer.restart()
        }
    }
}
