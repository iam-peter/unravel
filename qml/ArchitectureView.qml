pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// The architecture as it is, not as it was meant: modules (folders),
// files or classes, layered by what depends on what, cycles in red. Beside
// it the modules' coupling after Robert C. Martin.
Item {
    id: view
    property bool active: false

    DependencyScene {
        id: deps
        active: view.active
        level: levelBar.currentIndex
        moduleDepth: depthBox.value
        filter: filterBox.currentIndex > 0 ? filterBox.currentText : ""
        showExternal: externalBox.checked
        includesOnly: includesBox.checked
        cyclesOnly: cyclesBox.checked
        vertical: verticalBox.checked
        maxNodes: 160
        font: Theme.uiFont
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            Layout.fillWidth: true
            padding: Styles.Primitives.spacing.sp075
            background: Rectangle { color: Theme.s.background_default }
            Flow {
                width: parent.width
                spacing: Styles.Primitives.spacing.sp100
                TabBar {
                    id: levelBar
                    Layout.minimumWidth: implicitWidth
                    TabButton { text: qsTr("Modules"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Files"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Classes"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                }
                RowLayout {
                visible: levelBar.currentIndex === 0
                Label { text: qsTr("Folder depth"); color: Theme.textMuted }
                SpinBox {
                    id: depthBox
                    visible: levelBar.currentIndex === 0
                    from: 0; to: 6; value: 0
                    styles: [Styles.FieldStyle.small]
                    implicitWidth: 96
                    textFromValue: v => v === 0 ? qsTr("all") : String(v)
                }
                }
                RowLayout {
                visible: levelBar.currentIndex > 0
                Label { text: qsTr("In"); color: Theme.textMuted }
                ComboBox {
                    id: filterBox
                    visible: levelBar.currentIndex > 0
                    styles: [Styles.FieldStyle.small]
                    Layout.preferredWidth: 200
                    model: [qsTr("Whole project")].concat(deps.moduleNames)
                }
                }
                RowLayout {
                Switch { id: includesBox; text: qsTr("Includes only"); font: Theme.smallFont; visible: levelBar.currentIndex < 2 }
                Switch { id: cyclesBox; text: qsTr("Cycles only"); font: Theme.smallFont }
                Switch { id: externalBox; text: qsTr("Libraries"); font: Theme.smallFont }
                Switch { id: verticalBox; text: qsTr("Top down"); checked: true; font: Theme.smallFont }
                }
                Label {
                    height: 28
                    verticalAlignment: Text.AlignVCenter
                    text: deps.summary
                    color: deps.cycleCount > 0 ? Theme.danger : Theme.textMuted
                    font: Theme.smallFont
                }
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal
            handle: SplitterHandle {}

            GraphCanvas {
                id: canvas
                SplitView.fillWidth: true
                scene: deps
                showLegend: false
                onEdgeClicked: edge => {
                    const i = deps.edgeInfo(edge)
                    refs.title = i.fromName + " → " + i.toName
                    refs.references = deps.linkReferences(edge)
                    refs.visible = true
                }
                onNodeActivated: node => {
                    if (node.module !== undefined && node.module !== "") {
                        // A module: look inside it at its files
                        levelBar.currentIndex = 1
                        const i = deps.moduleNames.indexOf(node.module)
                        filterBox.currentIndex = i >= 0 ? i + 1 : 0
                    }
                }
                ReferenceList {
                    id: refs
                    visible: false
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    width: Math.min(460, parent.width * 0.5)
                    height: Math.min(380, parent.height - 80)
                    onCloseRequested: visible = false
                }
            }

            // Coupling of the modules
            Pane {
                SplitView.preferredWidth: 330
                SplitView.minimumWidth: 220
                visible: levelBar.currentIndex === 0
                padding: Styles.Primitives.spacing.sp100
                background: Rectangle { color: Theme.s.background_elevated }
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6
                    Label { text: qsTr("Module coupling"); font.weight: Styles.Primitives.fontWeight.semiBold }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.textMuted
                        font: Theme.smallFont
                        text: qsTr("Instability I = Ce/(Ca+Ce): 0 for what everything depends on, 1 for what depends on everything. Abstractness A: the share of abstract classes. Distance D = |A+I−1|: far from 0 means stable and concrete (hard to change) or unstable and abstract (useless).")
                    }
                    // The main sequence chart
                    Canvas {
                        id: chart
                        Layout.fillWidth: true
                        Layout.preferredHeight: width
                        property var metrics: view.active && Session.revision >= 0 ? Session.moduleMetrics() : []
                        onMetricsChanged: requestPaint()
                        onWidthChanged: requestPaint()
                        Connections { target: Theme; function onDarkChanged() { chart.requestPaint() } }
                        onPaint: {
                            const ctx = getContext("2d")
                            const w = width, h = height, m = 24
                            ctx.clearRect(0, 0, w, h)
                            ctx.strokeStyle = Theme.cardBorder
                            ctx.strokeRect(m, 4, w - m - 4, h - m - 4)
                            ctx.strokeStyle = Theme.success
                            ctx.setLineDash([4, 4])
                            ctx.beginPath(); ctx.moveTo(m, 4); ctx.lineTo(w - 4, h - m); ctx.stroke()
                            ctx.setLineDash([])
                            ctx.fillStyle = Theme.textMuted
                            ctx.font = "10px sans-serif"
                            ctx.fillText("I →", w - 26, h - 8)
                            ctx.fillText("A", 4, 14)
                            for (const mm of metrics) {
                                if (mm.name.startsWith("<")) continue
                                const x = m + mm.instability * (w - m - 4)
                                const y = 4 + (1 - mm.abstractness) * (h - m - 4)
                                const r = 3 + Math.sqrt(mm.lines) / 6
                                ctx.fillStyle = mm.distance > 0.7 ? Theme.danger : mm.distance > 0.4 ? Theme.warning : Theme.accent
                                ctx.globalAlpha = 0.55
                                ctx.beginPath(); ctx.arc(x, y, Math.min(r, 18), 0, 2 * Math.PI); ctx.fill()
                                ctx.globalAlpha = 1
                                ctx.fillStyle = Theme.text
                                ctx.fillText(mm.name.split("/").pop(), x + 6, y - 4)
                            }
                        }
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: chart.metrics
                        ScrollBar.vertical: ScrollBar {}
                        header: RowLayout {
                            width: ListView.view.width
                            Label { text: qsTr("Module"); Layout.fillWidth: true; color: Theme.textMuted; font: Theme.smallFont }
                            Label { text: "Ca"; Layout.preferredWidth: 26; color: Theme.textMuted; font: Theme.smallFont }
                            Label { text: "Ce"; Layout.preferredWidth: 26; color: Theme.textMuted; font: Theme.smallFont }
                            Label { text: "I"; Layout.preferredWidth: 34; color: Theme.textMuted; font: Theme.smallFont }
                            Label { text: "D"; Layout.preferredWidth: 34; color: Theme.textMuted; font: Theme.smallFont }
                        }
                        delegate: RowLayout {
                            required property var modelData
                            width: ListView.view.width
                            Label { text: modelData.name; Layout.fillWidth: true; elide: Text.ElideMiddle; font: Theme.smallFont }
                            Label { text: modelData.afferent; Layout.preferredWidth: 26; font: Theme.smallFont }
                            Label { text: modelData.efferent; Layout.preferredWidth: 26; font: Theme.smallFont }
                            Label { text: modelData.instability.toFixed(2); Layout.preferredWidth: 34; font: Theme.smallFont }
                            Label {
                                text: modelData.distance.toFixed(2); Layout.preferredWidth: 34; font: Theme.smallFont
                                color: modelData.distance > 0.7 ? Theme.danger : modelData.distance > 0.4 ? Theme.warning : Theme.text
                            }
                        }
                    }
                }
            }
        }
    }
}
