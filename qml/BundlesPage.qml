pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// Hierarchical edge bundles (Holten, as in SolidSX): the project's folders,
// files and classes around a circle, every dependency inside it, bundled
// along the hierarchy. Point at something for its own.
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
                    id: leafBar
                    Layout.minimumWidth: implicitWidth
                    TabButton { text: qsTr("Classes and functions"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Files"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                }
                Label { text: qsTr("Bundling"); color: Theme.textMuted }
                Slider { id: beta; from: 0; to: 1; value: 0.85; implicitWidth: 120 }
                Switch { id: byKind; text: qsTr("Colour by kind"); font: Theme.smallFont
                    ToolTip.visible: hovered; ToolTip.text: qsTr("Otherwise green at the user, red at what is used") }
                KindToggles { id: kinds }
                FoldMenu {}
                Item { Layout.fillWidth: true }
                Label {
                    text: qsTr("%1 nodes · %2 links").arg(bundles.leafCount).arg(bundles.linkCount)
                    color: Theme.textMuted
                    font: Theme.smallFont
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.canvas
            clip: true
            BundleView {
                id: bundles
                anchors.fill: parent
                bundling: beta.value
                leafLevel: leafBar.currentIndex
                colorByKind: byKind.checked
                kinds: kinds.selected
                colors: Theme.viewColors
                activeSymbol: Session.activeSymbol
                HoverHandler {
                    id: hover
                    onPointChanged: bundles.hover(point.position.x, point.position.y)
                }
                TapHandler {
                    onTapped: point => {
                        const it = bundles.itemAt(point.position.x, point.position.y)
                        if (it.folder !== undefined) Session.unfold(it.folder)
                        else if (it.symbol !== undefined && it.symbol >= 0) Session.activate(it.symbol)
                        else if (it.file !== undefined && it.file >= 0) Session.activateFile(it.file, 0)
                    }
                }
                WheelHandler {
                    onWheel: event => bundles.zoom = bundles.zoom * Math.pow(1.12, event.angleDelta.y / 120)
                }
            }
            Pane {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 10
                visible: bundles.hoveredInfo.name !== undefined
                padding: 8
                background: Rectangle { color: Styles.Primitives.alpha(Theme.card, 0.95); border.color: Theme.cardBorder; radius: Theme.radius }
                ColumnLayout {
                    spacing: 2
                    Label { text: bundles.hoveredInfo.name || ""; font.weight: Styles.Primitives.fontWeight.semiBold }
                    Label { text: (bundles.hoveredInfo.kind || "") + "  ·  " + (bundles.hoveredInfo.file || ""); color: Theme.textMuted; font: Theme.smallFont }
                    Label {
                        text: qsTr("uses %1 · used %2 times").arg(bundles.hoveredInfo.outgoing || 0).arg(bundles.hoveredInfo.incoming || 0)
                        font: Theme.smallFont
                    }
                }
            }
            RowLayout {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 10
                visible: !byKind.checked
                spacing: 6
                Label { text: qsTr("user"); font: Theme.smallFont; color: Theme.textMuted }
                Rectangle {
                    width: 90; height: 6; radius: 3
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: Theme.viewColors.source }
                        GradientStop { position: 1; color: Theme.viewColors.target }
                    }
                }
                Label { text: qsTr("used"); font: Theme.smallFont; color: Theme.textMuted }
            }
        }
    }
}
