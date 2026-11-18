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
            Item { Layout.fillWidth: true }
            Button {
                text: Hotspots.busy ? qsTr("Cancel") : qsTr("Refresh")
                styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                enabled: Session.hasModel
                onClicked: Hotspots.busy ? Hotspots.cancel() : Hotspots.refresh()
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
            text: qsTr("Non-merge commits; renames are counted at each pathname, not followed. Uncommitted edits are excluded.")
            color: Theme.textMuted
            font: Theme.smallFont
            wrapMode: Text.Wrap
        }
    }
}
