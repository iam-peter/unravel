pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// The project: its files and what they declare, its namespaces, the bookmarks
Pane {
    id: sidebar

    padding: 0
    background: Rectangle { color: Theme.s.background_elevated }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: tabs
            Layout.fillWidth: true
            Layout.margins: Styles.Primitives.spacing.sp075
            TabButton { text: qsTr("Files"); styles: [Styles.TabStyle.small]; width: (tabs.width - 4) / 3 }
            TabButton { text: qsTr("Symbols"); styles: [Styles.TabStyle.small]; width: (tabs.width - 4) / 3 }
            TabButton { text: qsTr("Starred"); styles: [Styles.TabStyle.small]; width: (tabs.width - 4) / 3 }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            ProjectTreeView { mode: ProjectTree.Files }
            ProjectTreeView { mode: ProjectTree.Namespaces }

            ListView {
                id: bookmarks
                clip: true
                model: Session.bookmarks
                ScrollBar.vertical: ScrollBar {}
                Label {
                    anchors.centerIn: parent
                    width: parent.width - 32
                    visible: bookmarks.count === 0
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.textMuted
                    text: qsTr("No bookmarks yet. The star in the tool bar (Ctrl+D) bookmarks the active symbol.")
                }
                delegate: ItemDelegate {
                    id: bm
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    styles: [Styles.ItemStyle.listRow]
                    highlighted: modelData.id === Session.activeSymbol
                    onClicked: Session.activate(modelData.id)
                    contentItem: RowLayout {
                        spacing: 6
                        Icon { name: Theme.kindIcon(bm.modelData.kind); size: 14; color: Theme.kindColor(bm.modelData.kind) }
                        ColumnLayout {
                            spacing: 0
                            Layout.fillWidth: true
                            Label { text: bm.modelData.qualified; elide: Text.ElideMiddle; Layout.fillWidth: true }
                            Label { text: bm.modelData.location; color: Theme.textMuted; font: Theme.smallFont; elide: Text.ElideLeft; Layout.fillWidth: true }
                        }
                        IconButton {
                            iconName: "x"
                            visible: bm.hovered
                            onClicked: Session.removeBookmark(bm.index)
                        }
                    }
                }
            }
        }
    }

    component ProjectTreeView: TreeView {
        id: tree
        property alias mode: treeModel.mode
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        model: ProjectTree { id: treeModel }

        // Show where the active symbol is
        Connections {
            target: Session
            function onActiveChanged() { tree.reveal() }
        }
        function reveal() {
            if (!tree.visible) return
            const idx = Session.activeSymbol >= 0 ? treeModel.indexOfSymbol(Session.activeSymbol)
                                                  : treeModel.indexOfFile(Session.activeFile)
            if (!idx.valid) return
            tree.expandToIndex(idx)
            Qt.callLater(() => {
                const r = tree.rowAtIndex(idx)
                if (r >= 0) tree.positionViewAtRow(r, TableView.Contain)
            })
        }

        delegate: Item {
            id: node
            required property TreeView treeView
            required property bool isTreeNode
            required property bool expanded
            required property bool hasChildren
            required property int depth
            required property int row
            required property string name
            required property string detail
            required property string iconName
            required property int kind
            required property int symbol
            required property int file
            required property bool unused
            required property bool external

            readonly property bool active: (symbol >= 0 && symbol === Session.activeSymbol)
                                         || (symbol < 0 && file >= 0 && file === Session.activeFile)
            implicitWidth: tree.width
            implicitHeight: 26

            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                radius: Styles.Primitives.radius.r3
                color: node.active ? Theme.s.background_accent
                     : hover.hovered ? Qt.tint(Theme.s.background_elevated, Qt.rgba(0.5, 0.5, 0.5, 0.12)) : "transparent"
            }
            HoverHandler { id: hover }
            TapHandler {
                onTapped: {
                    if (node.symbol >= 0) Session.activate(node.symbol)
                    else if (node.file >= 0) Session.activateFile(node.file, 0)
                    else node.treeView.toggleExpanded(node.row)
                }
                onDoubleTapped: if (node.hasChildren) node.treeView.toggleExpanded(node.row)
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8 + node.depth * 14
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
                    name: node.kind === -3 && node.expanded ? "folder-open" : node.iconName
                    size: 15
                    color: node.kind === -3 ? Theme.fileColor : Theme.kindColor(node.kind)
                }
                Label {
                    text: node.name
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    color: node.external ? Theme.textMuted : Theme.text
                    font.strikeout: node.unused
                    font.weight: node.active ? Styles.Primitives.fontWeight.semiBold : Styles.Primitives.fontWeight.regular
                }
                Label {
                    visible: node.kind === -2
                    text: node.detail
                    color: Theme.textMuted
                    font: Theme.smallFont
                }
            }
            ToolTip.visible: hover.hovered && node.detail !== "" && node.kind !== -2
            ToolTip.delay: 800
            ToolTip.text: node.detail
        }
    }
}
