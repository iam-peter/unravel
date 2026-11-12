pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import IgittStyle as Styles
import Unravel

// Sourcetrail's graph: the active symbol in the middle with its members,
// what uses it to the left, what it uses to the right, bases above and
// subclasses below
Item {
    id: view
    property bool active: false

    SymbolGraph {
        id: graph
        active: view.active
        symbol: Session.activeSymbol
        file: Session.activeSymbol < 0 ? Session.activeFile : -1
        font: Theme.uiFont
        grouping: groupBox.currentIndex
        showExternal: externalSwitch.checked
        kindFilter: kinds.selected
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            Layout.fillWidth: true
            padding: Styles.Primitives.spacing.sp075
            background: Rectangle { color: Theme.s.background_default }
            RowLayout {
                width: parent.width
                spacing: Styles.Primitives.spacing.sp100
                Label { text: qsTr("Group"); color: Theme.textMuted }
                ComboBox {
                    id: groupBox
                    styles: [Styles.FieldStyle.small]
                    model: [qsTr("None"), qsTr("By namespace"), qsTr("By file")]
                    implicitWidth: 140
                }
                ToolSeparator {}
                KindToggles { id: kinds }
                FoldMenu {}
                ToolSeparator {}
                Switch {
                    id: externalSwitch
                    text: qsTr("Libraries")
                    checked: false
                    font: Theme.smallFont
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Show what comes from outside the project (the standard library, Qt…)")
                }
                Switch {
                    text: qsTr("All members")
                    checked: graph.allMembers
                    onToggled: graph.allMembers = checked
                    font: Theme.smallFont
                }
                Item { Layout.fillWidth: true }
                IconButton {
                    iconName: "download"
                    onClicked: exportMenu.popup()
                    ToolTip.visible: hovered && !exportMenu.visible
                    ToolTip.text: qsTr("Export the graph")
                    Menu {
                        id: exportMenu
                        MenuItem { text: qsTr("Save as SVG…"); onTriggered: { saveDialog.svg = true; saveDialog.open() } }
                        MenuItem { text: qsTr("Save as PNG…"); onTriggered: { saveDialog.svg = false; saveDialog.open() } }
                        MenuItem { text: qsTr("Copy as SVG"); onTriggered: Session.copyToClipboard(graph.toSvg(Theme.viewColors)) }
                    }
                }
            }
        }

        GraphCanvas {
            id: canvas
            Layout.fillWidth: true
            Layout.fillHeight: true
            scene: graph
            onEdgeClicked: edge => {
                refs.title = (() => { const i = graph.edgeInfo(edge); return i.fromName + " → " + i.toName })()
                refs.references = graph.edgeReferences(edge)
                refs.visible = true
            }

            ReferenceList {
                id: refs
                visible: false
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 10
                width: Math.min(440, parent.width * 0.45)
                height: Math.min(360, parent.height - 80)
                onCloseRequested: visible = false
            }
            Label {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: 10
                text: graph.summary
                color: Theme.textMuted
                font: Theme.smallFont
            }
        }
    }

    FileDialog {
        id: saveDialog
        property bool svg: true
        fileMode: FileDialog.SaveFile
        defaultSuffix: svg ? "svg" : "png"
        nameFilters: svg ? [qsTr("SVG (*.svg)")] : [qsTr("PNG (*.png)")]
        onAccepted: {
            if (svg) {
                Session.writeText(selectedFile, graph.toSvg(Theme.viewColors))
            } else {
                canvas.grabToImage(result => result.saveToFile(Session.toLocalPath(selectedFile)))
            }
        }
    }
}
