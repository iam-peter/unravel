pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

Pane {
    padding: Styles.Primitives.spacing.sp075
    background: Rectangle { color: Theme.s.background_default }
    ColumnLayout {
        width: parent.width
        spacing: Styles.Primitives.spacing.sp050
        RowLayout {
            Layout.fillWidth: true
            Label { text: qsTr("Git history"); font.weight: Styles.Primitives.fontWeight.semiBold }
            SpinBox {
                from: 1
                to: 3650
                value: Hotspots.days
                editable: true
                styles: [Styles.FieldStyle.small]
                implicitWidth: 120
                onValueModified: Hotspots.days = value
            }
            Label { text: qsTr("days"); color: Theme.textMuted }
            TextField {
                Layout.preferredWidth: 130
                styles: [Styles.FieldStyle.small]
                text: Hotspots.revision
                placeholderText: qsTr("Revision")
                onEditingFinished: Hotspots.revision = text
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Blame active code")
                styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                enabled: Session.activeInfo.file !== undefined
                onClicked: {
                    Hotspots.loadBlame(Session.activeInfo.file, Session.activeInfo.line || 1)
                    blameDialog.open()
                }
            }
            Button {
                text: Hotspots.busy ? qsTr("Cancel") : qsTr("Refresh")
                styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                enabled: Session.hasModel
                onClicked: Hotspots.busy ? Hotspots.cancel() : Hotspots.refresh(true)
            }
        }
        Label {
            Layout.fillWidth: true
            text: Hotspots.summary
            color: Hotspots.error !== "" ? Theme.danger : Theme.textMuted
            font: Theme.smallFont
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            text: Hotspots.followRenames
                  ? qsTr("Non-merge commits; Git --follow infers renames per tracked file (slower, heuristic). Uncommitted edits are excluded.")
                  : qsTr("Non-merge commits; renames are counted at each pathname, not followed. Uncommitted edits are excluded.")
            color: Theme.textMuted
            font: Theme.smallFont
            wrapMode: Text.Wrap
        }
        CheckBox {
            text: qsTr("Follow renames (Git heuristic; slower)")
            checked: Hotspots.followRenames
            onToggled: Hotspots.followRenames = checked
        }
    }
    Dialog {
        id: blameDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(900, parent.width - 40)
        height: Math.min(600, parent.height - 40)
        title: qsTr("Line attribution (code context, not contributor scoring)")
        modal: true
        standardButtons: Dialog.Close
        onClosed: Hotspots.cancelBlame()
        contentItem: ScrollView {
            TextArea {
                readOnly: true
                text: Hotspots.blameText
                font.family: "monospace"
                wrapMode: TextEdit.NoWrap
                selectByMouse: true
            }
        }
    }
}
