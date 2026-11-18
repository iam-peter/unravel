pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Unravel

Item {
    id: panel
    property bool active: false
    property var flow: ControlFlow
    readonly property var diagram: {
        const evidence = flow.blocks
        return evidence.length ? flow.diagram() : ({})
    }
    property int selectedBlock: -1
    onDiagramChanged: selectedBlock = -1

    function analyzeActive() {
        const info = Session.activeInfo
        if (info && info.isFunction && info.defined)
            flow.analyze(info.file, info.line, info.qualified)
    }

    Connections {
        target: Session
        function onActiveChanged() {
            if (panel.active)
                panel.analyzeActive()
        }
    }
    onActiveChanged: if (active) analyzeActive()
    Component.onCompleted: if (active) analyzeActive()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: panel.flow.functionName.length
                      ? qsTr("Clang CFG · %1").arg(panel.flow.functionName)
                      : qsTr("Clang control-flow graph")
                elide: Text.ElideMiddle
                font.bold: true
            }
            Button {
                text: panel.flow.busy ? qsTr("Cancel") : qsTr("Analyze active function")
                enabled: panel.flow.busy || Session.activeInfo.isFunction === true
                onClicked: panel.flow.busy ? panel.flow.cancel() : panel.analyzeActive()
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: panel.flow.busy
                  ? qsTr("Parsing the selected translation unit with its compile-database arguments…")
                  : panel.flow.error.length ? panel.flow.error
                  : panel.flow.blocks.length
                    ? qsTr("%1 source blocks · %2 CFG edges. Exception paths and implicit destructors are omitted; coroutines are rejected.")
                      .arg(panel.flow.blocks.length).arg(panel.flow.edges.length)
                    : qsTr("No graph is shown until Clang constructs a CFG from the project compilation database.")
            color: panel.flow.error.length ? Theme.danger : Theme.textMuted
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            visible: panel.flow.blocks.length > 0
            TabButton { text: qsTr("Clang CFG") }
            TabButton { text: qsTr("Block evidence") }
            TabButton { text: qsTr("Experimental facts") }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: panel.flow.blocks.length === 0
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: panel.flow.blocks.length > 0
            currentIndex: tabs.currentIndex

            ColumnLayout {
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: panel.diagram.error || qsTr("Actual Clang blocks and directed successors · click a block to select source · scroll to pan · diagram limit 150 blocks / 400 edges")
                }
                Flickable {
                    id: diagramView
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: Math.max(width, panel.diagram.width || 0)
                    contentHeight: Math.max(height, panel.diagram.height || 0)
                    ScrollBar.horizontal: ScrollBar {}
                    ScrollBar.vertical: ScrollBar {}

                    Canvas {
                        id: edgeCanvas
                        x: diagramView.contentX
                        y: diagramView.contentY
                        width: diagramView.width
                        height: diagramView.height
                        property var graph: panel.diagram
                        onGraphChanged: requestPaint()
                        onXChanged: requestPaint()
                        onYChanged: requestPaint()
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            ctx.clearRect(0, 0, width, height)
                            ctx.translate(-x, -y)
                            const edges = graph.edges || []
                            ctx.font = "12px sans-serif"
                            for (const edge of edges) {
                                const points = edge.points
                                if (points.length < 2)
                                    continue
                                ctx.strokeStyle = Theme.textMuted
                                ctx.fillStyle = Theme.textMuted
                                ctx.lineWidth = 1.5
                                ctx.beginPath()
                                ctx.moveTo(points[0].x, points[0].y)
                                for (let i = 1; i < points.length; ++i)
                                    ctx.lineTo(points[i].x, points[i].y)
                                ctx.stroke()
                                const end = points[points.length - 1]
                                const previous = points[points.length - 2]
                                const angle = Math.atan2(end.y - previous.y, end.x - previous.x)
                                ctx.beginPath()
                                ctx.moveTo(end.x, end.y)
                                ctx.lineTo(end.x - 10 * Math.cos(angle - 0.4), end.y - 10 * Math.sin(angle - 0.4))
                                ctx.lineTo(end.x - 10 * Math.cos(angle + 0.4), end.y - 10 * Math.sin(angle + 0.4))
                                ctx.closePath()
                                ctx.fill()
                                const label = edge.kind + (edge.condition ? " · " + edge.condition
                                              : edge.caseValue ? " · " + edge.caseValue : "")
                                const middle = points[Math.floor((points.length - 1) / 2)]
                                const next = points[Math.min(points.length - 1, Math.floor((points.length - 1) / 2) + 1)]
                                ctx.fillText(label, (middle.x + next.x) / 2 + 5, (middle.y + next.y) / 2)
                            }
                        }
                    }
                    Repeater {
                        model: panel.diagram.nodes || []
                        delegate: Rectangle {
                            id: blockBox
                            required property var modelData
                            x: modelData.x
                            y: modelData.y
                            width: modelData.width
                            height: modelData.height
                            radius: 5
                            color: Theme.s.background_elevated
                            border.width: panel.selectedBlock === modelData.id ? 3 : 1
                            border.color: panel.selectedBlock === modelData.id ? Theme.functionColor : Theme.textMuted
                            Label {
                                anchors.fill: parent
                                anchors.margins: 8
                                wrapMode: Text.Wrap
                                maximumLineCount: 4
                                elide: Text.ElideRight
                                text: {
                                    const block = blockBox.modelData
                                    const location = block.source.line ? " · line " + block.source.line : ""
                                    const statements = block.statements.map(item => item.source.text || "").filter(text => text.length)
                                    return "BB" + block.id + location + (block.explicitReturn ? " · return" : "")
                                           + "\n" + (statements.join(" · ") || block.terminatorKind || "entry / exit / connector")
                                }
                            }
                            TapHandler {
                                onTapped: {
                                    panel.selectedBlock = blockBox.modelData.id
                                    panel.flow.openBlock(blockBox.modelData.index)
                                }
                            }
                        }
                    }
                }
            }

            SplitView {
                handle: SplitterHandle {}

                ListView {
                    id: blocks
                    SplitView.preferredWidth: 380
                    SplitView.minimumWidth: 220
                    clip: true
                    model: panel.flow.blocks
                    onCountChanged: currentIndex = count > 0 ? 0 : -1
                    delegate: ItemDelegate {
                        required property var modelData
                        required property int index
                        width: ListView.view.width
                        text: {
                            const source = modelData.source
                            const start = source.line ? ":" + source.line : ""
                            const tags = modelData.earlyReturn ? " · return" : ""
                            const snippets = modelData.statements.map(item => item.source.text || "")
                                                         .filter(text => text.length).join(" · ")
                            return "BB" + modelData.id + start + tags
                                   + (snippets.length ? " · " + snippets : "")
                        }
                        highlighted: ListView.isCurrentItem
                        ToolTip.visible: hovered
                        ToolTip.text: text
                        onClicked: {
                            blocks.currentIndex = index
                            panel.flow.openBlock(index)
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }

                ListView {
                    SplitView.fillWidth: true
                    clip: true
                    model: {
                        const selected = blocks.currentIndex
                        if (selected < 0 || selected >= panel.flow.blocks.length)
                            return []
                        const from = panel.flow.blocks[selected].id
                        return panel.flow.edges.filter(edge => edge.from === from)
                    }
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        text: {
                            const branch = modelData.condition || modelData.caseValue
                                         || qsTr("successor %1").arg(modelData.successorIndex)
                            return qsTr("BB%1 → BB%2 · %3 · %4")
                                   .arg(modelData.from).arg(modelData.to).arg(modelData.kind).arg(branch)
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }

            ColumnLayout {
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Experimental compiler observations only; no state provenance, ownership, borrow, or lifetime conclusions.")
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("G1 · Explicit enum assignments (prior state unknown)")
                    font.bold: true
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: {
                        const facts = panel.flow.experimentalFacts.facts
                        return facts ? facts.enumAssignments : []
                    }
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        text: qsTr("%1 ← %2::%3 = %4 · prior state unknown · %5:%6")
                              .arg(modelData.target).arg(modelData.enumType).arg(modelData.enumerator)
                              .arg(modelData.value).arg(modelData.assignment.file).arg(modelData.assignment.line)
                        onClicked: panel.flow.openLocation(modelData.assignment.file, modelData.assignment.line)
                    }
                    ScrollBar.vertical: ScrollBar {}
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("G2 · Pointer/reference type shapes (owner and borrow roles unknown)")
                    font.bold: true
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: {
                        const facts = panel.flow.experimentalFacts.facts
                        return facts ? facts.pointerReferenceTypeObservations : []
                    }
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        text: qsTr("%1 · %2 · %3 · no lifetime analysis")
                              .arg(modelData.name).arg(modelData.typeShape).arg(modelData.type)
                        onClicked: panel.flow.openLocation(modelData.declaration.file, modelData.declaration.line)
                    }
                    ScrollBar.vertical: ScrollBar {}
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: {
                        const facts = panel.flow.experimentalFacts.facts
                        const slices = facts ? facts.localDefUseSlices : null
                        return slices ? qsTr("G3 · No local def-use slice emitted: %1").arg(slices.reason)
                                      : qsTr("G3 · Local def-use slices are not implemented.")
                    }
                }
            }
        }
    }
}
