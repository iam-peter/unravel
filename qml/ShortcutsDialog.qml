pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

Dialog {
    id: dialog
    title: qsTr("Keyboard and mouse")
    modal: true
    closeButton: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 560

    ColumnLayout {
        width: parent.width
        spacing: Styles.Primitives.spacing.sp075
        Repeater {
            model: [
                ["Ctrl+K", qsTr("Search symbols and files (Tab: next category)")],
                ["Alt+Left / Alt+Right", qsTr("Back / forward through the symbols you looked at")],
                [qsTr("Mouse thumb buttons"), qsTr("Back / forward, as in a browser")],
                ["Ctrl+1 … Ctrl+9", qsTr("Graph, flow, architecture, matrix, bundles, treemap, metrics, review, changes")],
                ["Click, Ctrl, Shift", qsTr("Changes: pick one commit, add or remove one, pick a range")],
                ["Click", qsTr("A node, a member, a name in the code: make it the active symbol")],
                ["Click an edge", qsTr("The references behind it")],
                ["Right-click a node", qsTr("Open in the editor, copy the name, hide it")],
                ["Wheel / drag", qsTr("Zoom / pan the graphs")],
                ["Ctrl+D", qsTr("Bookmark the active symbol")],
                ["Ctrl+E", qsTr("Open the active symbol in the editor")],
                ["Ctrl+B / Ctrl+J", qsTr("Show or hide the project / the details and code")],
                ["F5", qsTr("Index again")],
                ["Ctrl+O", qsTr("Open a project")],
                ["Ctrl+,", qsTr("Settings")],
            ]
            delegate: RowLayout {
                id: r
                required property var modelData
                Layout.fillWidth: true
                spacing: Styles.Primitives.spacing.sp200
                Label { Layout.preferredWidth: 180; text: r.modelData[0]; font: Theme.monoFont; color: Theme.link }
                Label { Layout.fillWidth: true; text: r.modelData[1]; wrapMode: Text.Wrap }
            }
        }
    }
}
