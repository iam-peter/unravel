import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// The views, as tabs above them
Pane {
    id: bar

    property var views: []
    property int currentIndex: 0
    signal activated(int index)

    padding: Styles.Primitives.spacing.sp075
    background: Rectangle {
        color: Theme.s.background_default
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.cardBorder
        }
    }

    RowLayout {
        width: parent.width
        spacing: Styles.Primitives.spacing.sp100

        TabBar {
            id: tabs
            Layout.fillWidth: false
            Layout.minimumWidth: implicitWidth
            currentIndex: bar.currentIndex
            // Icons only when the names don't fit
            readonly property bool compact: bar.width < 940
            Repeater {
                model: bar.views
                TabButton {
                    required property var modelData
                    required property int index
                    text: modelData.name
                    width: implicitWidth
                    display: tabs.compact && index !== bar.currentIndex ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
                    icon.source: Icons.source(modelData.icon)
                    onClicked: bar.activated(index)
                    ToolTip.visible: hovered
                    ToolTip.delay: 700
                    ToolTip.text: modelData.name + ": " + modelData.tip + "  (Ctrl+" + (index + 1) + ")"
                }
            }
        }
        Item { Layout.fillWidth: true }
    }
}
