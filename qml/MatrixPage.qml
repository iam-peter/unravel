pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// A dependency structure matrix: row i uses column j. Ordered with what
// is depended on first, everything sits below the diagonal in a layered
// design; marks above it, framed in red, are cycles.
Item {
    id: view
    property bool active: false

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Pane {
            Layout.fillWidth: true
            padding: Styles.Primitives.spacing.sp075
            background: Rectangle { color: Theme.s.background_default }
            RowLayout {
                width: parent.width
                spacing: Styles.Primitives.spacing.sp100
                TabBar {
                    id: levelBar
                    Layout.minimumWidth: implicitWidth
                    TabButton { text: qsTr("Modules"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Files"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Classes"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                }
                Label { text: qsTr("Folder depth"); color: Theme.textMuted; visible: levelBar.currentIndex === 0 }
                SpinBox {
                    id: depthBox
                    visible: levelBar.currentIndex === 0
                    from: 0; to: 6; value: 0
                    styles: [Styles.FieldStyle.small]
                    implicitWidth: 96
                    textFromValue: v => v === 0 ? qsTr("all") : String(v)
                }
                FoldMenu {}
                Item { Layout.fillWidth: true }
                Label {
                    color: Theme.textMuted
                    font: Theme.smallFont
                    text: {
                        const h = matrix.hoveredInfo
                        if (!h || h.from === undefined) return qsTr("Row uses column · %n nodes", "", matrix.count)
                        if (h.diagonal) return h.from
                        return h.from + " → " + h.to + ": " + (h.count + h.includes > 0
                               ? (h.kinds || "") + (h.includes ? qsTr(", %1 includes").arg(h.includes) : "") + (h.cyclic ? qsTr(" · in a cycle") : "")
                               : qsTr("no dependency"))
                    }
                }
            }
        }
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            handle: SplitterHandle {}
            Flickable {
                id: flick
                SplitView.fillWidth: true
                clip: true
                contentWidth: Math.max(width, matrix.contentWidth)
                contentHeight: Math.max(height, matrix.contentHeight)
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                ScrollBar.horizontal: ScrollBar {}
                Rectangle { width: flick.contentWidth; height: flick.contentHeight; color: Theme.canvas }
                // The size of the viewport, kept in view: it paints only what shows
                MatrixView {
                    id: matrix
                    x: flick.contentX
                    y: flick.contentY
                    width: flick.width
                    height: flick.height
                    viewX: flick.contentX
                    viewY: flick.contentY
                    level: levelBar.currentIndex
                    moduleDepth: depthBox.value
                    colors: Theme.viewColors
                    HoverHandler {
                        onPointChanged: matrix.hover(point.position.x, point.position.y)
                        onHoveredChanged: if (!hovered) matrix.hover(-1, -1)
                    }
                    TapHandler {
                        onTapped: point => {
                            const n = matrix.nodeInfo(point.position.x, point.position.y)
                            if (n.folder !== undefined) { Session.unfold(n.folder); return }
                            if (n.symbol !== undefined) { Session.activate(n.symbol); return }
                            if (n.file !== undefined) { Session.activateFile(n.file, 0); return }
                            matrix.select(point.position.x, point.position.y)
                            refs.references = matrix.selectedReferences()
                            const h = matrix.hoveredInfo
                            refs.title = h.from + " → " + h.to
                            refs.visible = matrix.selectedLink >= 0
                        }
                    }
                }
            }
            ReferenceList {
                id: refs
                visible: false
                SplitView.preferredWidth: 380
                onCloseRequested: visible = false
            }
        }
    }
}
