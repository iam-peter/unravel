pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// What clang reported while indexing: errors make the picture incomplete
Dialog {
    id: dialog

    title: qsTr("Compiler errors and warnings")
    modal: true
    closeButton: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(980, parent.width - 80)
    height: Math.min(640, parent.height - 80)

    property var items: visible ? Session.diagnostics() : []

    ColumnLayout {
        anchors.fill: parent
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            CheckBox { id: errorsOnly; text: qsTr("Errors only"); checked: true }
            TextField { id: filter; placeholderText: qsTr("Filter"); styles: [Styles.FieldStyle.small]; Layout.preferredWidth: 260 }
            Item { Layout.fillWidth: true }
            Label { text: qsTr("%n shown", "", list.count); color: Theme.textMuted }
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dialog.items.filter(d => (!errorsOnly.checked || d.severity >= 2)
                                             && (filter.text === "" || (d.message + d.location).toLowerCase().indexOf(filter.text.toLowerCase()) >= 0))
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                id: row
                required property var modelData
                width: ListView.view.width
                styles: [Styles.ItemStyle.listRow, Styles.ItemStyle.small]
                onClicked: { Session.activateFile(modelData.file, modelData.line); dialog.close() }
                contentItem: RowLayout {
                    spacing: 8
                    Rectangle { implicitWidth: 8; implicitHeight: 8; radius: 4; color: row.modelData.severity >= 2 ? Theme.danger : Theme.warning }
                    Label { text: row.modelData.message; Layout.fillWidth: true; elide: Text.ElideRight }
                    Label { text: row.modelData.location; color: Theme.textMuted; font: Theme.smallFont; Layout.maximumWidth: 320; elide: Text.ElideLeft }
                }
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: modelData.message + (modelData.unit ? qsTr("\nwhile compiling %1").arg(modelData.unit) : "")
            }
        }
    }
}
