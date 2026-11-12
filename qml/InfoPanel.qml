pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// What the active symbol (or file) is: its name and signature, where it
// is, its measurements and what the review found about it
Pane {
    id: panel
    signal showView(string id)

    readonly property var info: Session.activeInfo
    readonly property bool isFile: info.isFile === true
    readonly property var cls: !isFile && info.isType ? Session.classMetrics(info.id) : ({})

    padding: Styles.Primitives.spacing.sp150
    background: Rectangle { color: Theme.s.background_elevated }

    Flickable {
        anchors.fill: parent
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            width: parent.width
            spacing: 6

            Label {
                visible: panel.info.id === undefined
                text: qsTr("Pick a symbol: search (Ctrl+K), the tree, or any view")
                color: Theme.textMuted
            }

            RowLayout {
                visible: panel.info.id !== undefined
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    implicitWidth: 26
                    implicitHeight: 26
                    radius: 4
                    color: Qt.tint(Theme.card, Styles.Primitives.alpha(Theme.kindColor(panel.isFile ? -2 : panel.info.kind), 0.35))
                    Icon {
                        anchors.centerIn: parent
                        name: Theme.kindIcon(panel.isFile ? -2 : panel.info.kind)
                        size: 15
                        color: Theme.text
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    TextEdit {
                        Layout.fillWidth: true
                        readOnly: true
                        selectByMouse: true
                        text: panel.isFile ? panel.info.relative || "" : panel.info.qualified || ""
                        font.pixelSize: 15
                        font.weight: Styles.Primitives.fontWeight.semiBold
                        color: Theme.text
                        wrapMode: TextEdit.WrapAnywhere
                    }
                    Label {
                        text: panel.isFile ? qsTr("file") + (panel.info.generated ? qsTr(" · generated") : "")
                                           : panel.info.kindName + (panel.info.external ? qsTr(" · from a library") : "")
                        color: Theme.kindColor(panel.isFile ? -2 : panel.info.kind)
                        font: Theme.smallFont
                    }
                }
                IconButton {
                    iconName: "external-link"
                    onClicked: Session.openInEditor(panel.info.file !== undefined ? (panel.isFile ? panel.info.id : panel.info.file) : -1,
                                                    panel.info.line || 1)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Open in the editor (Ctrl+E)")
                }
                IconButton {
                    iconName: "copy"
                    onClicked: Session.copyToClipboard(panel.isFile ? panel.info.path : panel.info.qualified)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Copy the name")
                }
            }

            TextEdit {
                visible: !panel.isFile && (panel.info.signature || "") !== "" && panel.info.signature !== panel.info.name
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: panel.info.signature || ""
                font: Theme.monoFont
                color: Theme.text
                wrapMode: TextEdit.Wrap
            }
            TextEdit {
                visible: !panel.isFile && (panel.info.type || "") !== "" && panel.info.isVariable
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: panel.info.type || ""
                font: Theme.monoFont
                color: Theme.s.syntax_type
                wrapMode: TextEdit.Wrap
            }

            // Badges
            Flow {
                Layout.fillWidth: true
                spacing: 4
                visible: panel.info.id !== undefined
                Repeater {
                    model: panel.isFile ? [
                        { t: qsTr("%1 lines").arg(panel.info.lines), on: true },
                        { t: qsTr("not compiled"), on: !panel.info.indexed && !panel.info.header, c: Theme.warning },
                        { t: qsTr("%1 errors").arg(panel.info.errors), on: panel.info.errors > 0, c: Theme.danger },
                    ] : [
                        { t: qsTr("entry point"), on: panel.info.entryPoint, c: Theme.success },
                        { t: qsTr("virtual"), on: panel.info.virtual && !panel.info.pure },
                        { t: qsTr("pure virtual"), on: panel.info.pure },
                        { t: qsTr("abstract"), on: panel.info.abstract },
                        { t: qsTr("static"), on: panel.info["static"] },
                        { t: qsTr("const"), on: panel.info["const"] && panel.info.isFunction },
                        { t: qsTr("template"), on: panel.info.template },
                        { t: qsTr("declared only"), on: !panel.info.defined && !panel.info.external, c: Theme.warning },
                        { t: qsTr("looks unused"), on: panel.info.unused, c: Theme.warning },
                        { t: qsTr("from a macro"), on: panel.info.fromMacro },
                    ]
                    Rectangle {
                        required property var modelData
                        visible: modelData.on === true
                        width: badge.implicitWidth + 12
                        height: badge.implicitHeight + 4
                        radius: height / 2
                        color: Styles.Primitives.alpha(modelData.c || Theme.textMuted, 0.15)
                        border.color: Styles.Primitives.alpha(modelData.c || Theme.textMuted, 0.4)
                        Label {
                            id: badge
                            anchors.centerIn: parent
                            text: parent.modelData.t
                            font: Theme.smallFont
                            color: parent.modelData.c || Theme.text
                        }
                    }
                }
            }

            Label {
                visible: !panel.isFile && panel.info.location !== undefined && panel.info.location !== ""
                text: (panel.info.defined ? qsTr("defined in ") : qsTr("declared in ")) + (panel.info.location || "")
                color: Theme.link
                font: Theme.smallFont
                TapHandler { onTapped: Session.openInEditor(panel.info.file, panel.info.line) }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }

            // Measurements
            GridLayout {
                Layout.fillWidth: true
                visible: panel.info.id !== undefined && !panel.isFile
                columns: 4
                columnSpacing: 12
                rowSpacing: 2
                Repeater {
                    model: {
                        const i = panel.info
                        if (i.id === undefined || panel.isFile) return []
                        let m = []
                        if (i.isFunction)
                            m = [[qsTr("Lines"), i.lines, 80], [qsTr("Complexity"), i.complexity, 15], [qsTr("Cognitive"), i.cognitive, 25],
                                 [qsTr("Nesting"), i.nesting, 4], [qsTr("Parameters"), i.params, 6], [qsTr("Statements"), i.statements, 0]]
                        else if (i.isType && panel.cls.methods !== undefined)
                            m = [[qsTr("Methods"), panel.cls.methods, 30], [qsTr("Fields"), panel.cls.fields, 0], [qsTr("WMC"), panel.cls.wmc, 80],
                                 [qsTr("Inheritance"), panel.cls.depth, 5], [qsTr("Subclasses"), panel.cls.children, 0],
                                 [qsTr("Uses"), panel.cls.coupling, 20], [qsTr("Used by"), panel.cls.usedBy, 0],
                                 [qsTr("Cohesion"), Math.round(panel.cls.cohesion * 100) + "%", 0]]
                        if (i.isFunction)
                            m.push([qsTr("Callers"), i.callers, 0], [qsTr("Calls"), i.callees, 0])
                        else if (!(i.isType && panel.cls.methods !== undefined))
                            m.push([qsTr("Used by"), i.fanIn, 0], [qsTr("Uses"), i.fanOut, 0])
                        m.push([qsTr("References"), i.references, 0])
                        return m
                    }
                    RowLayout {
                        required property var modelData
                        spacing: 4
                        Label { text: modelData[0]; color: Theme.textMuted; font: Theme.smallFont }
                        Label {
                            text: modelData[1]
                            font.family: Theme.monoFont.family
                            font.pointSize: Theme.monoFont.pointSize
                            font.weight: Styles.Primitives.fontWeight.semiBold
                            color: modelData[2] > 0 && Number(modelData[1]) >= modelData[2] ? Theme.danger : Theme.text
                        }
                    }
                }
            }

            // What the review says about it
            Repeater {
                model: panel.info.findings || []
                RowLayout {
                    required property string modelData
                    Layout.fillWidth: true
                    spacing: 6
                    Icon { name: "triangle-alert"; size: 13; color: Theme.warning }
                    Label { text: modelData; wrapMode: Text.Wrap; Layout.fillWidth: true; font: Theme.smallFont }
                }
            }

            RowLayout {
                visible: panel.info.id !== undefined && !panel.isFile
                spacing: 4
                Button {
                    visible: panel.info.isFunction === true
                    styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                    icon.source: Icons.source("workflow")
                    text: qsTr("Call flow")
                    onClicked: panel.showView("flow")
                }
                Button {
                    styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                    icon.source: Icons.source("network")
                    text: qsTr("Graph")
                    onClicked: panel.showView("graph")
                }
                Button {
                    visible: panel.info.parent !== undefined && panel.info.parent >= 0
                    styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                    icon.source: Icons.source("arrow-up")
                    text: qsTr("Up to %1").arg((panel.info.parentName || "").split("::").pop())
                    onClicked: Session.activate(panel.info.parent)
                }
            }
        }
    }
}
