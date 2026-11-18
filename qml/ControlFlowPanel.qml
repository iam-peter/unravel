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
        const evidence = flow.blocks;
        return evidence.length ? flow.diagram(collapse.checked) : ({});
    }
    property int selectedBlock: -1
    onDiagramChanged: selectedBlock = -1

    function analyzeActive() {
        const info = Session.activeInfo;
        if (info && info.isFunction && info.defined)
            flow.analyze(info.file, info.line, info.qualified);
    }

    Connections {
        target: Session
        function onActiveChanged() {
            if (panel.active)
                panel.analyzeActive();
        }
    }
    onActiveChanged: if (active)
        analyzeActive()
    Component.onCompleted: if (active)
        analyzeActive()

    ColumnLayout {
        CheckBox {
            id: collapse
            visible: panel.flow.blocks.length > 0
            text: qsTr("Collapse straight-line regions (retain all block evidence)")
        }
        anchors.fill: parent
        anchors.margins: 12

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: panel.flow.functionName.length ? qsTr("Clang CFG · %1").arg(panel.flow.functionName) : qsTr("Clang control-flow graph")
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
            text: panel.flow.busy ? qsTr("Parsing the selected translation unit with its compile-database arguments…") : panel.flow.error.length ? panel.flow.error : panel.flow.blocks.length ? qsTr("%1 source blocks · %2 CFG edges. Exception paths and implicit destructors are omitted; coroutines are rejected.").arg(panel.flow.blocks.length).arg(panel.flow.edges.length) : qsTr("No graph is shown until Clang constructs a CFG from the project compilation database.")
            color: panel.flow.error.length ? Theme.danger : Theme.textMuted
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            visible: panel.flow.blocks.length > 0
            TabButton {
                text: qsTr("Clang CFG")
            }
            TabButton {
                text: qsTr("Block evidence")
            }
            TabButton {
                text: qsTr("Experimental facts")
            }
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
                            const ctx = getContext("2d");
                            ctx.reset();
                            ctx.clearRect(0, 0, width, height);
                            ctx.translate(-x, -y);
                            const edges = graph.edges || [];
                            ctx.font = "12px sans-serif";
                            for (const edge of edges) {
                                const points = edge.points;
                                if (points.length < 2)
                                    continue;
                                ctx.strokeStyle = Theme.textMuted;
                                ctx.fillStyle = Theme.textMuted;
                                ctx.lineWidth = 1.5;
                                ctx.beginPath();
                                ctx.moveTo(points[0].x, points[0].y);
                                for (let i = 1; i < points.length; ++i)
                                    ctx.lineTo(points[i].x, points[i].y);
                                ctx.stroke();
                                const end = points[points.length - 1];
                                const previous = points[points.length - 2];
                                const angle = Math.atan2(end.y - previous.y, end.x - previous.x);
                                ctx.beginPath();
                                ctx.moveTo(end.x, end.y);
                                ctx.lineTo(end.x - 10 * Math.cos(angle - 0.4), end.y - 10 * Math.sin(angle - 0.4));
                                ctx.lineTo(end.x - 10 * Math.cos(angle + 0.4), end.y - 10 * Math.sin(angle + 0.4));
                                ctx.closePath();
                                ctx.fill();
                                const label = edge.kind + (edge.condition ? " · " + edge.condition : edge.caseValue ? " · " + edge.caseValue : "");
                                const middle = points[Math.floor((points.length - 1) / 2)];
                                const next = points[Math.min(points.length - 1, Math.floor((points.length - 1) / 2) + 1)];
                                ctx.fillText(label, (middle.x + next.x) / 2 + 5, (middle.y + next.y) / 2);
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
                                    const block = blockBox.modelData;
                                    const location = block.source.line ? " · line " + block.source.line : "";
                                    const statements = block.statements.map(item => item.source.text || "").filter(text => text.length);
                                    return (block.blockIds ? "BB " + block.blockIds.join(", ") : "BB" + block.id) + location + (block.explicitReturn ? " · return" : "") + "\n" + (statements.join(" · ") || block.terminatorKind || "entry / exit / connector");
                                }
                            }
                            TapHandler {
                                onTapped: {
                                    panel.selectedBlock = blockBox.modelData.id;
                                    panel.flow.openBlock(blockBox.modelData.index);
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
                            const source = modelData.source;
                            const start = source.line ? ":" + source.line : "";
                            const tags = modelData.earlyReturn ? " · return" : "";
                            const snippets = modelData.statements.map(item => item.source.text || "").filter(text => text.length).join(" · ");
                            return "BB" + modelData.id + start + tags + (snippets.length ? " · " + snippets : "");
                        }
                        highlighted: ListView.isCurrentItem
                        ToolTip.visible: hovered
                        ToolTip.text: text
                        onClicked: {
                            blocks.currentIndex = index;
                            panel.flow.openBlock(index);
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }

                ColumnLayout {
                    SplitView.fillWidth: true
                    Label {
                        text: qsTr("Successors")
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: {
                            const selected = blocks.currentIndex;
                            if (selected < 0 || selected >= panel.flow.blocks.length)
                                return [];
                            const from = panel.flow.blocks[selected].id;
                            return panel.flow.edges.filter(edge => edge.from === from);
                        }
                        delegate: ItemDelegate {
                            required property var modelData
                            width: ListView.view.width
                            text: {
                                const branch = modelData.condition || modelData.caseValue || qsTr("successor %1").arg(modelData.successorIndex);
                                return qsTr("BB%1 → BB%2 · %3 · %4").arg(modelData.from).arg(modelData.to).arg(modelData.kind).arg(branch);
                            }
                        }
                        ScrollBar.vertical: ScrollBar {}
                    }
                    Label {
                        text: qsTr("Calls in selected block · select to open call site")
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: {
                            const selected = blocks.currentIndex;
                            if (selected < 0 || selected >= panel.flow.blocks.length)
                                return [];
                            return panel.flow.blocks[selected].statements.filter(statement => statement.call).map(statement => statement.call);
                        }
                        delegate: ItemDelegate {
                            required property var modelData
                            width: ListView.view.width
                            text: modelData.qualifiedName + " · " + modelData.source.line
                            onClicked: panel.flow.openLocation(modelData.source.file, modelData.source.line)
                        }
                        ScrollBar.vertical: ScrollBar {}
                    }
                }
            }

            ColumnLayout {
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Experimental, function-scoped evidence. Unknown origins and effects stay visible. No lifetime safety, invalid-transition, or sanitization proof.")
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("G1 · Enum/member writes · only immediate equality guards constrain prior state")
                    font.bold: true
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: {
                        const facts = panel.flow.experimentalFacts.facts;
                        return facts ? facts.enumAssignments : [];
                    }
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        text: qsTr("%1 ← %2::%3 = %4 · prior %5 · %6:%7").arg(modelData.target).arg(modelData.enumType).arg(modelData.enumerator).arg(modelData.value).arg(modelData.priorState).arg(modelData.assignment.file).arg(modelData.assignment.line)
                        onClicked: panel.flow.openLocation(modelData.assignment.file, modelData.assignment.line)
                    }
                    ScrollBar.vertical: ScrollBar {}
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Qt state-machine registrations · potential transitions, not execution")
                    font.bold: true
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: {
                        const facts = panel.flow.experimentalFacts.facts;
                        return facts ? facts.qtStateTransitions : [];
                    }
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        text: modelData.api + " · " + (modelData.from ? modelData.from.text : qsTr("unknown origin")) + " → " + modelData.to.text
                        onClicked: panel.flow.openLocation(modelData.source.file, modelData.source.line)
                    }
                    ScrollBar.vertical: ScrollBar {}
                }

                RowLayout {
                    Button {
                        text: qsTr("State graph")
                        onClicked: experiments.showEvidence("stateTransitions")
                    }
                    Button {
                        text: qsTr("Ownership graph")
                        onClicked: experiments.showEvidence("ownership")
                    }
                    Button {
                        text: qsTr("Local data-flow slices")
                        onClicked: experiments.showEvidence("localDefUseSlices")
                    }
                    Button {
                        text: qsTr("Copy evidence JSON")
                        onClicked: Session.copyToClipboard(JSON.stringify(panel.flow.experimentalFacts, null, 2))
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: {
                        const facts = panel.flow.experimentalFacts.facts;
                        const slices = facts ? facts.localDefUseSlices : null;
                        return slices ? (slices.policy || slices.reason) : qsTr("Local data-flow evidence unavailable.");
                    }
                }
            }
        }
    }

    Dialog {
        id: experiments
        parent: Overlay.overlay
        modal: true
        property string kind: "ownership"
        property string seed: ""
        property bool forward: false
        property string stateTarget: ""
        readonly property var facts: panel.flow.experimentalFacts.facts || ({})
        readonly property var evidence: facts[kind] || ({})
        readonly property var selected: {
            if (kind === "localDefUseSlices" && seed.length)
                return panel.flow.slice(seed, forward);
            if (kind === "stateTransitions" && stateTarget.length)
                return {
                    nodes: (evidence.nodes || []).filter(node => node.stateVariable === stateTarget),
                    edges: (evidence.edges || []).filter(edge => edge.stateVariable === stateTarget)
                };
            return evidence;
        }
        readonly property var graph: panel.flow.evidenceDiagram(selected.nodes || [], selected.edges || [])
        function showEvidence(which) {
            kind = which;
            seed = "";
            stateTarget = "";
            visible = true;
        }
        title: kind === "ownership" ? qsTr("Experimental ownership relationships") : kind === "stateTransitions" ? qsTr("Experimental state transitions") : qsTr("Experimental local data-flow slice")
        anchors.centerIn: parent
        width: Math.min(parent.width - 24, 1000)
        height: Math.min(parent.height - 24, 720)
        standardButtons: Dialog.Close
        onFactsChanged: seed = ""
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: experiments.evidence.policy || experiments.evidence.reason || qsTr("Evidence unavailable")
            }
            RowLayout {
                visible: experiments.kind === "stateTransitions"
                ComboBox {
                    Layout.fillWidth: true
                    model: [
                        {
                            id: "",
                            name: qsTr("All enum fields/locals and Qt registrations")
                        }
                    ].concat(experiments.evidence.targets || []).concat([
                        {
                            id: "qt",
                            name: qsTr("Qt registrations")
                        }
                    ])
                    textRole: "name"
                    valueRole: "id"
                    onActivated: experiments.stateTarget = currentValue
                }
            }
            RowLayout {
                visible: experiments.kind === "localDefUseSlices"
                ComboBox {
                    Layout.fillWidth: true
                    model: experiments.evidence.nodes || []
                    textRole: "label"
                    valueRole: "id"
                    onActivated: experiments.seed = currentValue
                }
                CheckBox {
                    text: qsTr("Forward (unchecked: backward)")
                    onToggled: experiments.forward = checked
                }
                Button {
                    text: qsTr("All evidence")
                    onClicked: experiments.seed = ""
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: experiments.selected.error || ((experiments.selected.unknownInfluence ? qsTr("Unknown influence is present. ") : "") + (experiments.selected.truncated ? qsTr("Truncated at 1000 nodes. ") : "") + qsTr("%1 nodes · %2 evidenced edges").arg((experiments.selected.nodes || []).length).arg((experiments.selected.edges || []).length) + (experiments.kind === "ownership" ? qsTr(" · %1 unresolved transfer sites").arg((experiments.evidence.unknownTransfers || []).length) : ""))
            }
            TabBar {
                id: evidenceTabs
                Layout.fillWidth: true
                TabButton {
                    text: qsTr("Graph")
                }
                TabButton {
                    text: qsTr("Nodes")
                }
                TabButton {
                    text: qsTr("Edges")
                }
            }
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: evidenceTabs.currentIndex
                ColumnLayout {
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: experiments.graph.error || qsTr("Select a node to open source. Graph limits: 150 nodes / 400 edges; complete evidence remains in the lists.")
                    }
                    Flickable {
                        id: factView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentWidth: Math.max(width, experiments.graph.width || 0)
                        contentHeight: Math.max(height, experiments.graph.height || 0)
                        ScrollBar.vertical: ScrollBar {}
                        ScrollBar.horizontal: ScrollBar {}
                        Canvas {
                            x: factView.contentX
                            y: factView.contentY
                            width: factView.width
                            height: factView.height
                            property var graph: experiments.graph
                            onGraphChanged: requestPaint()
                            onXChanged: requestPaint()
                            onYChanged: requestPaint()
                            onWidthChanged: requestPaint()
                            onHeightChanged: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d");
                                ctx.reset();
                                ctx.clearRect(0, 0, width, height);
                                ctx.translate(-x, -y);
                                ctx.strokeStyle = Theme.textMuted;
                                ctx.fillStyle = Theme.textMuted;
                                ctx.font = "12px sans-serif";
                                for (const edge of graph.edges || []) {
                                    const points = edge.points;
                                    if (points.length < 2)
                                        continue;
                                    ctx.beginPath();
                                    ctx.moveTo(points[0].x, points[0].y);
                                    for (let i = 1; i < points.length; ++i)
                                        ctx.lineTo(points[i].x, points[i].y);
                                    ctx.stroke();
                                    const end = points[points.length - 1];
                                    const prev = points[points.length - 2];
                                    const angle = Math.atan2(end.y - prev.y, end.x - prev.x);
                                    ctx.beginPath();
                                    ctx.moveTo(end.x, end.y);
                                    ctx.lineTo(end.x - 9 * Math.cos(angle - 0.4), end.y - 9 * Math.sin(angle - 0.4));
                                    ctx.lineTo(end.x - 9 * Math.cos(angle + 0.4), end.y - 9 * Math.sin(angle + 0.4));
                                    ctx.closePath();
                                    ctx.fill();
                                    ctx.fillText(edge.role || "data", end.x + 4, end.y - 12);
                                }
                            }
                        }
                        Repeater {
                            model: experiments.graph.nodes || []
                            delegate: Rectangle {
                                required property var modelData
                                x: modelData.x
                                y: modelData.y
                                width: modelData.width
                                height: modelData.height
                                radius: 5
                                color: Theme.s.background_elevated
                                border.color: modelData.unknownInfluence || modelData.kind === "unknown-object" ? Theme.danger : Theme.textMuted
                                Label {
                                    anchors.fill: parent
                                    anchors.margins: 7
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 4
                                    elide: Text.ElideRight
                                    text: (parent.modelData.label || parent.modelData.evidenceId) + "\n" + parent.modelData.kind + " · " + ((parent.modelData.source || {}).text || "")
                                }
                                TapHandler {
                                    onTapped: panel.flow.openLocation(parent.modelData.source.file, parent.modelData.source.line)
                                }
                            }
                        }
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: experiments.selected.nodes || []
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        text: (modelData.label || modelData.id) + " · " + modelData.kind + (modelData.unknownInfluence ? qsTr(" · UNKNOWN influence") : "") + " · " + ((modelData.source || {}).text || "")
                        onClicked: panel.flow.openLocation(modelData.source.file, modelData.source.line)
                        ToolTip.visible: hovered
                        ToolTip.text: text
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: experiments.selected.edges || []
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        text: modelData.from + " → " + modelData.to + " · " + (modelData.role || qsTr("data dependency"))
                        onClicked: panel.flow.openLocation(modelData.source.file, modelData.source.line)
                        ToolTip.visible: hovered
                        ToolTip.text: text
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }
        }
    }
}
