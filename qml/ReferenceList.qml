pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// The references behind an edge or a cell: where in the code, from what
// to what. Clicking one shows that line in the code view.
Pane {
    id: panel

    property string title: ""
    property var references: []
    signal closeRequested()

    padding: Styles.Primitives.spacing.sp100
    background: Rectangle {
        color: Theme.card
        border.color: Theme.cardBorder
        radius: Theme.radius
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: panel.title
                font.weight: Styles.Primitives.fontWeight.semiBold
                elide: Text.ElideMiddle
            }
            Label {
                text: qsTr("%n references", "", panel.references.length)
                color: Theme.textMuted
                font: Theme.smallFont
            }
            IconButton { iconName: "x"; onClicked: panel.closeRequested() }
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: panel.references
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                id: ref
                required property var modelData
                width: ListView.view.width
                styles: [Styles.ItemStyle.listRow, Styles.ItemStyle.small]
                onClicked: Session.activateReference(modelData.id)
                contentItem: ColumnLayout {
                    spacing: 0
                    RowLayout {
                        spacing: 6
                        Rectangle { implicitWidth: 10; implicitHeight: 3; color: Theme.edgeColor(ref.modelData.kind) }
                        Label {
                            Layout.fillWidth: true
                            text: ref.modelData.fromName + "  →  " + ref.modelData.toName
                            elide: Text.ElideMiddle
                            font: Theme.smallFont
                        }
                    }
                    Label {
                        text: ref.modelData.location + "  ·  " + ref.modelData.kindName + (ref.modelData.write ? qsTr(" (writes)") : "")
                        color: Theme.textMuted
                        font: Theme.smallFont
                        elide: Text.ElideLeft
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
}
