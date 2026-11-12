pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import IgittStyle as Styles
import Unravel

// The folded folders: each shown as one node in the graphs. Unfold one or
// all; fold what looks like third-party code at once.
ToolButton {
    id: button

    styles: [Styles.ButtonStyle.small]
    icon.source: Icons.source("package")
    icon.color: Session.foldedFolders.length ? Theme.accent : palette.buttonText
    text: Session.foldedFolders.length ? qsTr("Folded (%1)").arg(Session.foldedFolders.length) : qsTr("Fold")
    onClicked: menu.popup(button, 0, button.height)
    ToolTip.visible: hovered && !menu.visible
    ToolTip.delay: 600
    ToolTip.text: qsTr("Folders shown as one node; folders fold from the tree or a node's menu too")

    Menu {
        id: menu
        MenuItem {
            readonly property var found: Session.vendorFolders.filter(f => !Session.isFolded(f))
            enabled: found.length > 0
            text: found.length ? qsTr("Fold third-party code: %1").arg(found.join(", ")) : qsTr("No third-party folders found")
            onTriggered: Session.foldVendorFolders()
        }
        MenuSeparator {}
        Instantiator {
            model: Session.foldedFolders
            delegate: MenuItem {
                required property string modelData
                text: qsTr("Unfold %1").arg(modelData)
                onTriggered: Session.unfold(modelData)
            }
            onObjectAdded: (index, object) => menu.insertItem(2 + index, object)
            onObjectRemoved: (index, object) => menu.removeItem(object)
        }
        MenuItem {
            enabled: Session.foldedFolders.length > 0
            text: qsTr("Unfold all")
            onTriggered: Session.unfoldAll()
        }
    }
}
