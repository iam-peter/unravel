pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// One node of a graph scene: a card with the symbol (or file, module,
// class) and the member rows that take part in the edges
Rectangle {
    id: card

    required property var modelData
    required property int index
    property GraphScene scene
    signal activated(var node)

    readonly property var d: modelData
    readonly property bool isBundle: d.bundle === true
    readonly property bool compact: d.compact === true
    readonly property bool hot: scene && scene.highlightNode === index

    x: d.x
    y: d.y
    width: d.w
    height: d.h
    radius: Theme.radius
    color: isBundle ? Theme.card
         : d.kind === -4 ? Qt.tint(Theme.card, Styles.Primitives.alpha(Theme.namespaceColor, 0.22))
         : d.kind === -3 ? Qt.tint(Theme.card, Styles.Primitives.alpha(Theme.namespaceColor, 0.06 + 0.22 * (d.sizeFraction || 0)))
         : Theme.nodeFill(d.kind, d.center === true || d.active === true)
    border.width: d.center || d.target || hot || d.cyclic || d.changed ? 2 : 1
    border.color: d.cyclic ? Theme.danger
                : d.added ? Theme.added
                : d.changed ? Theme.changed
                : d.target ? Theme.success
                : d.center ? Theme.accent
                : hot ? Theme.text
                : Styles.Primitives.alpha(Theme.kindColor(d.kind), 0.7)
    opacity: d.context ? 0.62 : d.external ? 0.78 : 1

    // Hatching for what isn't in the indexed sources, as Sourcetrail does: a
    // tiled texture, drawn by the GPU
    Image {
        anchors.fill: parent
        anchors.margins: 1
        visible: card.d.external === true
        source: "qrc:/qt/qml/Unravel/images/hatch.svg"
        fillMode: Image.Tile
        sourceSize: Qt.size(8, 8)
        opacity: 0.35
        smooth: false
    }

    HoverHandler {
        id: cardHover
        onHoveredChanged: if (card.scene) card.scene.highlightNode = hovered ? card.index : (card.scene.highlightNode === card.index ? -1 : card.scene.highlightNode)
    }

    function activateNode() {
        if (d.folder !== undefined && d.folder !== "") {
            Session.unfold(d.folder)   // a folded folder opens again
            return
        }
        if (isBundle) {
            if (scene && scene.expandSide)
                scene.expandSide(d.side)
            return
        }
        if (d.symbol >= 0)
            Session.activate(d.symbol)
        else if (d.file >= 0)
            Session.activateFile(d.file, 0)
        activated(d)
    }

    // Header
    Item {
        id: header
        width: parent.width
        height: card.compact ? parent.height : 30
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onTapped: card.activateNode()
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: menu.popup()
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 8
            spacing: 0
            Item { Layout.fillHeight: true }
            RowLayout {
                spacing: 6
                Layout.fillWidth: true
                Icon {
                    visible: !card.isBundle
                    name: Theme.kindIcon(card.d.kind)
                    size: 14
                    color: card.d.kind === -3 ? Theme.namespaceColor : Theme.kindColor(card.d.kind)
                }
                Label {
                    Layout.fillWidth: true
                    text: card.d.title
                    elide: Text.ElideMiddle
                    font.weight: Styles.Primitives.fontWeight.semiBold
                    font.italic: card.d.abstract === true
                    font.strikeout: card.d.unused === true
                    color: card.isBundle ? Theme.accent : Theme.text
                    horizontalAlignment: card.isBundle ? Text.AlignHCenter : Text.AlignLeft
                }
                Label {
                    visible: (card.d.hiddenCount || 0) > 0
                    text: "+" + card.d.hiddenCount
                    font: Theme.smallFont
                    color: Theme.accent
                    TapHandler { onTapped: if (card.scene) card.scene.allMembers = true }
                    ToolTip.visible: hiddenHover.hovered
                    ToolTip.text: qsTr("%n more members without edges here: show all", "", card.d.hiddenCount || 0)
                    HoverHandler { id: hiddenHover }
                }
                Label {
                    visible: (card.d.changedLines || 0) > 0 && card.d.changed === true
                    text: (card.d.added ? "+" : "±") + card.d.changedLines
                    font: Theme.smallFont
                    color: card.d.added ? Theme.added : Theme.changed
                    ToolTip.visible: changedHover.hovered
                    ToolTip.text: qsTr("%n lines changed in the selected commits", "", card.d.changedLines || 0)
                    HoverHandler { id: changedHover }
                }
                Label {
                    visible: card.d.complexity !== undefined && card.d.complexity >= 10
                    text: "cc " + card.d.complexity
                    font: Theme.smallFont
                    color: card.d.complexity >= 20 ? Theme.danger : Theme.warning
                }
            }
            Label {
                visible: card.compact && (card.d.subtitle || card.d.metric || "") !== ""
                Layout.fillWidth: true
                text: card.d.metric ? (card.d.subtitle ? card.d.subtitle + " · " : "") + card.d.metric : card.d.subtitle || ""
                font: Theme.smallFont
                color: Theme.textMuted
                elide: Text.ElideMiddle
            }
            Item { Layout.fillHeight: true }
        }
    }

    // Access sections
    Repeater {
        model: card.d.sections || []
        Label {
            required property var modelData
            x: 10
            y: modelData.y + 2
            text: modelData.label
            font.pixelSize: 10
            font.weight: Styles.Primitives.fontWeight.semiBold
            color: Theme.textMuted
        }
    }

    // Member rows
    Repeater {
        model: card.d.rows || []
        Rectangle {
            id: rowItem
            required property var modelData
            readonly property bool rowHot: card.scene && card.scene.highlightSymbol === modelData.symbol
            x: 4
            y: modelData.y
            width: card.width - 8
            height: modelData.h
            radius: Styles.Primitives.radius.r2
            color: modelData.active ? Styles.Primitives.alpha(Theme.accent, 0.22)
                 : rowHot || rowHover.hovered ? Styles.Primitives.alpha(Theme.text, 0.08)
                 : Styles.Primitives.alpha(Theme.card, 0.55)
            border.width: modelData.active ? 1 : 0
            border.color: Theme.accent
            HoverHandler {
                id: rowHover
                onHoveredChanged: if (card.scene) card.scene.highlightSymbol = hovered ? rowItem.modelData.symbol : -1
            }
            TapHandler { onTapped: Session.activate(rowItem.modelData.symbol) }
            TapHandler { acceptedButtons: Qt.RightButton; onTapped: { menu.target = rowItem.modelData.symbol; menu.popup() } }
            // Changed in the selected commits: a bar on the left
            Rectangle {
                visible: rowItem.modelData.changed === true
                width: 3
                height: parent.height - 4
                y: 2
                radius: 1.5
                color: rowItem.modelData.added ? Theme.added : Theme.changed
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 5
                Rectangle {
                    implicitWidth: 7
                    implicitHeight: 7
                    radius: Theme.kindIcon(rowItem.modelData.kind) === "function-square" ? 3.5 : 1
                    color: Theme.kindColor(rowItem.modelData.kind)
                }
                Label {
                    Layout.fillWidth: true
                    text: rowItem.modelData.name
                    opacity: rowItem.modelData.changed === false ? 0.6 : 1
                    elide: Text.ElideRight
                    font.italic: rowItem.modelData.virtual === true
                    font.strikeout: rowItem.modelData.unused === true
                    font.underline: rowItem.modelData["static"] === true
                    color: rowItem.modelData.external ? Theme.textMuted : Theme.text
                }
                Label {
                    visible: (rowItem.modelData.lines || 0) > 0
                    text: (rowItem.modelData.added ? "+" : "±") + rowItem.modelData.lines
                    font.pixelSize: 10
                    color: rowItem.modelData.added ? Theme.added : Theme.changed
                }
            }
            ToolTip.visible: rowHover.hovered
            ToolTip.delay: 700
            ToolTip.text: (modelData.signature || modelData.name) + (modelData.unused ? qsTr("\nlooks unused") : "")
        }
    }

    ToolTip.visible: cardHover.hovered && !card.isBundle && card.d.subtitle !== undefined && card.d.subtitle !== "" && !card.compact
                     && (card.d.rows || []).length === 0
    ToolTip.delay: 700
    ToolTip.text: card.isBundle ? card.d.subtitle : (card.d.subtitle || "")

    Menu {
        id: menu
        property int target: -1
        onClosed: target = -1
        readonly property int sym: target >= 0 ? target : card.d.symbol
        readonly property int foldAt: 6   // where the fold entries go
        MenuItem { text: qsTr("Show"); enabled: menu.sym >= 0 || card.d.file >= 0; onTriggered: menu.sym >= 0 ? Session.activate(menu.sym) : Session.activateFile(card.d.file, 0) }
        MenuItem {
            text: qsTr("Open in editor")
            enabled: menu.sym >= 0 || card.d.file >= 0
            onTriggered: {
                if (menu.sym >= 0) { const i = Session.symbolInfo(menu.sym); Session.openInEditor(i.file, i.line) }
                else Session.openInEditor(card.d.file, 1)
            }
        }
        MenuItem { text: qsTr("Copy name"); onTriggered: Session.copyToClipboard(menu.sym >= 0 ? Session.symbolInfo(menu.sym).qualified : card.d.title) }
        MenuSeparator {}
        // Folding: this node's folder and those above it, or unfolding it
        MenuItem {
            visible: card.d.folder !== undefined && card.d.folder !== ""
            height: visible ? implicitHeight : 0
            text: qsTr("Unfold %1").arg(card.d.folder || "")
            onTriggered: Session.unfold(card.d.folder)
        }
        MenuItem {
            visible: (card.d.module || "") !== ""
            height: visible ? implicitHeight : 0
            text: qsTr("Fold %1 into one node").arg(card.d.module || "")
            onTriggered: Session.fold(card.d.module)
        }
        Instantiator {
            model: menu.visible ? Session.foldersOfFile(menu.sym >= 0 ? Session.symbolInfo(menu.sym).file : card.d.file) : []
            delegate: MenuItem {
                required property string modelData
                text: qsTr("Fold %1 into one node").arg(modelData)
                onTriggered: Session.fold(modelData)
            }
            onObjectAdded: (index, object) => menu.insertItem(menu.foldAt + index, object)
            onObjectRemoved: (index, object) => menu.removeItem(object)
        }
        MenuItem { text: qsTr("Hide"); enabled: menu.sym >= 0 && card.scene !== null; onTriggered: card.scene.hide(menu.sym) }
        MenuItem { text: qsTr("Show hidden again"); enabled: card.scene && card.scene.hiddenSymbols.length > 0; onTriggered: card.scene.unhideAll() }
    }
}
