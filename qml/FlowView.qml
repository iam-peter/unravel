pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// How control flows: on the left the call tree of a function in the
// order of its code (or who calls it), on the right the same calls as a
// layered graph, or every path from it to another function
Item {
    id: view
    property bool active: false

    // The function shown, or the class whose methods are: the active
    // symbol when it is one
    property int root: -1
    property int target: -1
    function follow() {
        const i = Session.activeInfo
        if (i && (i.isFunction || i.isType))
            root = i.id
    }
    Component.onCompleted: follow()
    Connections {
        target: Session
        function onActiveChanged() { if (view.active) view.follow() }
        function onModelChanged() { view.target = -1; view.root = -1; view.follow() }
    }
    onActiveChanged: if (active) follow()

    CallGraphScene {
        id: calls
        active: view.active
        symbol: view.root
        target: view.target
        depth: depthBox.value
        callees: calleesBox.checked
        callers: callersBox.checked
        showExternal: externalBox.checked
        vertical: verticalBox.checked
        font: Theme.uiFont
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            Layout.fillWidth: true
            padding: Styles.Primitives.spacing.sp075
            background: Rectangle { color: Theme.s.background_default }
            // Wraps when the view is narrow
            Flow {
                width: parent.width
                spacing: Styles.Primitives.spacing.sp100
                Label {
                    height: 28
                    verticalAlignment: Text.AlignVCenter
                    width: Math.min(implicitWidth, 360)
                    text: {
                        if (view.root < 0)
                            return qsTr("Pick a function or a class")
                        const i = Session.symbolInfo(view.root)
                        return i.isFunction ? i.qualified + "()" : i.qualified
                    }
                    font.weight: Styles.Primitives.fontWeight.semiBold
                    elide: Text.ElideMiddle
                }
                RowLayout {
                CheckBox { id: calleesBox; text: qsTr("Calls"); checked: true; enabled: view.target < 0 }
                CheckBox { id: callersBox; text: qsTr("Callers"); enabled: view.target < 0 }
                }
                RowLayout {
                Label { text: qsTr("Depth"); color: Theme.textMuted }
                SpinBox { id: depthBox; from: 1; to: 12; value: 3; styles: [Styles.FieldStyle.small]; implicitWidth: 96 }
                }
                RowLayout {
                Switch { id: externalBox; text: qsTr("Libraries"); font: Theme.smallFont }
                Switch { id: verticalBox; text: qsTr("Top down"); font: Theme.smallFont }
                }
                RowLayout {
                Label { text: qsTr("Path to"); color: Theme.textMuted }
                ComboBox {
                    id: targetBox
                    Layout.preferredWidth: 220
                    styles: [Styles.FieldStyle.small]
                    editable: true
                    model: targetSearch
                    textRole: "qualified"
                    valueRole: "symbol"
                    displayText: view.target >= 0 ? Session.symbolInfo(view.target).qualified : editText
                    onEditTextChanged: if (editText !== Session.symbolInfo(view.target).qualified) targetSearch.query = editText
                    onActivated: index => {
                        const r = targetSearch.get(index)
                        if (r.symbol >= 0) view.target = r.symbol
                    }
                    SymbolSearch { id: targetSearch; category: SymbolSearch.Functions; limit: 40 }
                }
                IconButton {
                    iconName: "x"
                    visible: view.target >= 0
                    onClicked: { view.target = -1; targetBox.editText = "" }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Back to the call graph")
                }
                }
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal
            handle: SplitterHandle {}

            ColumnLayout {
                SplitView.preferredWidth: 340
                SplitView.minimumWidth: 220
                spacing: 0
                Pane {
                    Layout.fillWidth: true
                    padding: Styles.Primitives.spacing.sp075
                    background: Rectangle { color: Theme.s.background_elevated }
                    RowLayout {
                        width: parent.width
                        TabBar {
                            id: treeMode
                            Layout.minimumWidth: implicitWidth
                            TabButton { text: qsTr("Calls"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                            TabButton { text: qsTr("Callers"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                        }
                        Item { Layout.fillWidth: true }
                        Switch { id: implicitBox; text: qsTr("Implicit"); font: Theme.smallFont
                            ToolTip.visible: hovered; ToolTip.text: qsTr("Constructors, destructors and conversions the compiler calls") }
                    }
                }
                TreeView {
                    id: tree
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    model: CallTree {
                        id: callTree
                        root: view.root
                        callers: treeMode.currentIndex === 1
                        showImplicit: implicitBox.checked
                        showExternal: externalBox.checked
                        onRootChanged: Qt.callLater(() => tree.expandRecursively(-1, 1))
                    }
                    Component.onCompleted: Qt.callLater(() => tree.expandRecursively(-1, 1))
                    delegate: Item {
                        id: node
                        required property TreeView treeView
                        required property bool isTreeNode
                        required property bool expanded
                        required property bool hasChildren
                        required property int depth
                        required property int row
                        required property string name
                        required property int symbol
                        required property int kind
                        required property int line
                        required property int count
                        required property bool recursive
                        required property bool isVirtual
                        required property bool external
                        required property bool dispatch
                        required property int complexity
                        required property int ref
                        required property string detail
                        implicitWidth: tree.width
                        implicitHeight: 26
                        Rectangle {
                            anchors.fill: parent
                            anchors.leftMargin: 4
                            anchors.rightMargin: 4
                            radius: Styles.Primitives.radius.r3
                            color: node.symbol === Session.activeSymbol ? Theme.s.background_accent
                                 : rowHover.hovered ? Qt.tint(Theme.s.background_elevated, Qt.rgba(0.5, 0.5, 0.5, 0.12)) : "transparent"
                        }
                        HoverHandler { id: rowHover }
                        TapHandler {
                            onTapped: {
                                if (node.ref >= 0) Session.activateReference(node.ref)
                                calls.highlightSymbol = node.symbol
                            }
                            onDoubleTapped: Session.activate(node.symbol)
                        }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6 + node.depth * 14
                            anchors.rightMargin: 8
                            spacing: 4
                            Item {
                                implicitWidth: 16
                                implicitHeight: 16
                                Icon {
                                    anchors.centerIn: parent
                                    visible: node.hasChildren
                                    name: node.expanded ? "chevron-down" : "chevron-right"
                                    size: 14
                                    color: Theme.textMuted
                                }
                                TapHandler { onTapped: node.treeView.toggleExpanded(node.row) }
                            }
                            Icon {
                                // A class at the top, its methods below
                                readonly property bool callable: node.kind >= 8 && node.kind <= 12
                                name: node.recursive ? "repeat" : node.dispatch ? "git-fork" : callable ? "function-square" : Theme.kindIcon(node.kind)
                                size: 14
                                color: node.recursive ? Theme.danger : node.dispatch ? Theme.namespaceColor : Theme.kindColor(node.kind)
                            }
                            Label {
                                text: node.name + (node.kind >= 8 && node.kind <= 12 ? "()" : "")
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                font.italic: node.isVirtual
                                color: node.external ? Theme.textMuted : Theme.text
                            }
                            Label {
                                visible: node.count > 1
                                text: "×" + node.count
                                font: Theme.smallFont
                                color: Theme.textMuted
                            }
                            Label {
                                visible: node.complexity >= 10
                                text: "cc " + node.complexity
                                font: Theme.smallFont
                                color: node.complexity >= 20 ? Theme.danger : Theme.warning
                            }
                            Label {
                                visible: node.line > 0
                                text: ":" + node.line
                                font: Theme.smallFont
                                color: Theme.textMuted
                            }
                        }
                        ToolTip.visible: rowHover.hovered
                        ToolTip.delay: 700
                        ToolTip.text: (node.dispatch ? qsTr("May run instead (virtual dispatch)\n") : "")
                                      + (node.recursive ? qsTr("Already on this path: recursion\n") : "")
                                      + node.detail + qsTr("\nClick: show the call · double-click: make it active")
                    }
                }
            }

            GraphCanvas {
                id: canvas
                SplitView.fillWidth: true
                scene: calls
                showLegend: false
                onEdgeClicked: edge => {
                    const i = calls.edgeInfo(edge)
                    refs.title = i.fromName + " → " + i.toName
                    refs.references = calls.edgeReferences(edge)
                    refs.visible = true
                }
                Label {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.margins: 10
                    text: calls.summary
                    color: Theme.textMuted
                    font: Theme.smallFont
                }
                ReferenceList {
                    id: refs
                    visible: false
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    width: Math.min(420, parent.width * 0.5)
                    height: Math.min(320, parent.height - 80)
                    onCloseRequested: visible = false
                }
            }
        }
    }
}
