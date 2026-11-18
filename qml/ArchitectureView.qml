pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
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

        Label {
            visible: Contracts.configured && Contracts.valid
            Layout.fillWidth: true
            padding: 6
            font: Theme.smallFont
            color: Theme.textMuted
            text: qsTr("Orange edges: architecture rule violations; red: cycles. Review > Architecture rules shows rule IDs and evidence.")
            wrapMode: Text.Wrap
        }
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
                FoldMenu {}
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
                    // The main sequence chart: items and a shape, drawn by the GPU
                    Item {
                        id: chart
                        Layout.fillWidth: true
                        Layout.preferredHeight: width
                        property var metrics: view.active && Session.revision >= 0 ? Session.moduleMetrics() : []
                        readonly property real m: 24
                        readonly property real plotW: width - m - 4
                        readonly property real plotH: height - m - 4
                        Rectangle {
                            x: chart.m; y: 4
                            width: chart.plotW; height: chart.plotH
                            color: "transparent"
                            border.color: Theme.cardBorder
                        }
                        Shape {
                            anchors.fill: parent
                            preferredRendererType: Shape.CurveRenderer
                            ShapePath {
                                strokeColor: Theme.success
                                strokeWidth: 1
                                strokeStyle: ShapePath.DashLine
                                dashPattern: [4, 4]
                                fillColor: "transparent"
                                startX: chart.m; startY: 4
                                PathLine { x: chart.m + chart.plotW; y: 4 + chart.plotH }
                            }
                        }
                        Label { x: 4; y: 4; text: "A"; font.pixelSize: 10; color: Theme.textMuted }
                        Label { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 2; text: "I →"; font.pixelSize: 10; color: Theme.textMuted }
                        Repeater {
                            model: chart.metrics.filter(mm => !mm.name.startsWith("<"))
                            Item {
                                required property var modelData
                                readonly property real r: Math.min(18, 3 + Math.sqrt(modelData.lines) / 6)
                                x: chart.m + modelData.instability * chart.plotW
                                y: 4 + (1 - modelData.abstractness) * chart.plotH
                                Rectangle {
                                    x: -parent.r; y: -parent.r
                                    width: 2 * parent.r; height: width
                                    radius: parent.r
                                    opacity: 0.55
                                    color: parent.modelData.distance > 0.7 ? Theme.danger : parent.modelData.distance > 0.4 ? Theme.warning : Theme.accent
                                }
                                Label {
                                    x: 6; y: -16
                                    text: parent.modelData.name.split("/").pop()
                                    font.pixelSize: 10
                                    color: Theme.text
                                }
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
