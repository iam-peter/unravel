pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// A button with a menu to show or hide the kinds of relations, each in
// its colour. `selected` is the list of RefKinds shown (empty means all).
ToolButton {
    id: toggles

    property var kinds: Theme.edgeKinds
    property var selected: []

    function isOn(kind) { return selected.length === 0 || selected.indexOf(kind) >= 0 }
    function toggle(kind) {
        const all = kinds.map(k => k.kind)
        let on = selected.length === 0 ? all.slice() : selected.slice()
        const i = on.indexOf(kind)
        if (i >= 0) on.splice(i, 1); else on.push(kind)
        selected = on.length === all.length ? [] : on
    }

    styles: [Styles.ButtonStyle.small]
    icon.source: Icons.source("filter")
    icon.color: selected.length ? Theme.accent : palette.buttonText
    text: selected.length ? qsTr("Relations (%1 of %2)").arg(selected.length).arg(kinds.length) : qsTr("Relations")
    onClicked: menu.popup(toggles, 0, toggles.height)

    Menu {
        id: menu
        Repeater {
            model: toggles.kinds
            MenuItem {
                id: item
                required property var modelData
                checkable: true
                checked: toggles.isOn(modelData.kind)
                text: modelData.name
                onTriggered: toggles.toggle(modelData.kind)
                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 34
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 3
                    radius: 1.5
                    color: Theme.edgeColor(item.modelData.kind)
                }
            }
        }
        MenuSeparator {}
        MenuItem { text: qsTr("Show all"); onTriggered: toggles.selected = [] }
    }
}
