pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// What the colours mean
Pane {
    id: legend
    padding: Styles.Primitives.spacing.sp100
    background: Rectangle {
        color: Styles.Primitives.alpha(Theme.card, 0.94)
        border.color: Theme.cardBorder
        radius: Theme.radius
    }
    GridLayout {
        columns: 2
        columnSpacing: 14
        rowSpacing: 4
        Repeater {
            model: [
                { c: Theme.typeColor, t: qsTr("Type"), box: true },
                { c: Theme.functionColor, t: qsTr("Function"), box: true },
                { c: Theme.variableColor, t: qsTr("Variable"), box: true },
                { c: Theme.namespaceColor, t: qsTr("Namespace"), box: true },
                { c: Theme.edgeColors.call, t: qsTr("Call") },
                { c: Theme.edgeColors.type, t: qsTr("Type use") },
                { c: Theme.edgeColors["var"], t: qsTr("Variable use") },
                { c: Theme.edgeColors.inherit, t: qsTr("Inheritance") },
                { c: Theme.edgeColors.override, t: qsTr("Override (dashed)") },
                { c: Theme.edgeColors.include, t: qsTr("Include") },
                { c: Theme.edgeColors.cycle, t: qsTr("In a cycle") },
            ]
            RowLayout {
                required property var modelData
                spacing: 6
                Rectangle {
                    width: modelData.box ? 12 : 16
                    height: modelData.box ? 12 : 3
                    radius: modelData.box ? 2 : 1.5
                    color: modelData.box ? Qt.tint(Theme.card, Styles.Primitives.alpha(modelData.c, 0.32)) : modelData.c
                    border.color: modelData.box ? modelData.c : "transparent"
                    border.width: modelData.box ? 2 : 0
                }
                Label { text: modelData.t; font: Theme.smallFont }
            }
        }
    }
}
