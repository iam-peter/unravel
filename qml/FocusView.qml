pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

ColumnLayout {
    id: view
    spacing: 0
    ReviewFocus {
        id: focus
        changedOnly: scope.currentIndex !== 0
        impactDepth: scope.currentIndex === 2 ? depth.value : 0
        historical: history.checked
        functionHistory: history.checked && functionHunks.checked
        showAll: all.checked
    }
    Pane {
        Layout.fillWidth: true
        padding: Styles.Primitives.spacing.sp075
        background: Rectangle { color: Theme.s.background_default }
        Flow {
            width: parent.width
            spacing: Styles.Primitives.spacing.sp100
            ComboBox {
                id: scope
                model: [qsTr("Whole project"), qsTr("Touched functions"), qsTr("Touched + potential dependents")]
                styles: [Styles.FieldStyle.small]
            }
            SpinBox { id: depth; visible: scope.currentIndex === 2; from: 1; to: 6; value: 1; implicitWidth: 100 }
            CheckBox { id: history; text: qsTr("Use Git history"); checked: true }
            CheckBox { id: functionHunks; text: qsTr("Estimate function hunks"); enabled: history.checked }
            CheckBox { id: all; text: qsTr("Show all (default: top 20)") }
            Button {
                text: qsTr("Copy Markdown")
                enabled: !focus.busy
                styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                onClicked: Session.copyToClipboard(focus.markdown())
            }
            Button {
                text: qsTr("Copy JSON")
                enabled: !focus.busy
                styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                onClicked: Session.copyToClipboard(focus.json())
            }
        }
    }
    HistoryBar { Layout.fillWidth: true; visible: history.checked }
    Label {
        Layout.fillWidth: true
        padding: 8
        wrapMode: Text.Wrap
        text: focus.summary + (scope.currentIndex !== 0 && Changes.busy ? qsTr(" | Computing selected changes...") : "")
              + (scope.currentIndex === 2 ? qsTr(" | Static dependency reach, depth %1; not proven execution impact.").arg(depth.value) : "")
        color: Theme.textMuted
    }
    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: focus
        ScrollBar.vertical: ScrollBar {}
        delegate: ItemDelegate {
            id: row
            required property int symbol
            required property string name
            required property string detail
            required property real priority
            required property int findings
            required property bool touched
            width: ListView.view.width
            styles: [Styles.ItemStyle.listRow]
            highlighted: symbol === Session.activeSymbol
            implicitHeight: content.implicitHeight + 16
            onClicked: Session.activate(symbol)
            contentItem: ColumnLayout {
                id: content
                Label {
                    Layout.fillWidth: true
                    text: row.priority.toFixed(2) + "  " + row.name
                    elide: Text.ElideMiddle
                    font.weight: Styles.Primitives.fontWeight.semiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: row.detail + qsTr(" | %1 findings").arg(row.findings)
                          + (row.touched ? qsTr(" | touched") : scope.currentIndex === 2 ? qsTr(" | potential dependent") : "")
                    font: Theme.smallFont
                    color: Theme.textMuted
                    wrapMode: Text.Wrap
                }
            }
        }
        Label {
            anchors.centerIn: parent
            visible: focus.count === 0
            text: qsTr("No eligible functions in this scope")
            color: Theme.textMuted
        }
    }
}
