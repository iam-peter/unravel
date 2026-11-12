pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// The code chosen commits changed, and nothing else: pick commits on the
// left (Ctrl adds, Shift a range); the middle shows the classes and
// functions they touched with their changed members and how they connect,
// if asked with what uses them; the right lists symbols, files and the
// review's findings in the change. The code view marks the changed lines.
Item {
    id: view
    property bool active: false
    property int anchorRow: -1

    ChangeScene {
        id: scene
        active: view.active
        context: contextBox.currentIndex
        vertical: verticalBox.checked
        font: Theme.uiFont
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal
        handle: SplitterHandle {}

        // --- The history ------------------------------------------------------
        Pane {
            SplitView.preferredWidth: 320
            SplitView.minimumWidth: 220
            padding: 0
            background: Rectangle { color: Theme.s.background_elevated }

            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Pane {
                    Layout.fillWidth: true
                    padding: Styles.Primitives.spacing.sp075
                    background: Item {}
                    ColumnLayout {
                        width: parent.width
                        spacing: 6
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: qsTr("History")
                                font.weight: Styles.Primitives.fontWeight.semiBold
                                Layout.fillWidth: true
                            }
                            IconButton {
                                iconName: "x"
                                visible: Changes.selected.length > 0
                                onClicked: Changes.clear()
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Clear the selection")
                            }
                            IconButton {
                                iconName: "refresh-cw"
                                enabled: Changes.available
                                onClicked: Changes.refresh()
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Read the history again")
                            }
                        }
                        TextField {
                            id: commitFilter
                            Layout.fillWidth: true
                            styles: [Styles.FieldStyle.small]
                            placeholderText: qsTr("Filter by message, author or hash")
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: Theme.textMuted
                            font: Theme.smallFont
                            text: Changes.available ? qsTr("Click a commit; Ctrl+click adds or removes one, Shift+click picks a range.")
                                                    : qsTr("The project isn't in a git repository (or git isn't installed).")
                        }
                    }
                }
                ListView {
                    id: commits
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    model: {
                        const f = commitFilter.text.toLowerCase()
                        const all = Changes.commits.map((c, i) => Object.assign({ row: i }, c))
                        if (f === "")
                            return all
                        return all.filter(c => c.workTree || (c.subject + " " + c.author + " " + c.hash).toLowerCase().indexOf(f) >= 0)
                    }
                    delegate: Rectangle {
                        id: commit
                        required property var modelData
                        width: ListView.view.width
                        height: 46
                        color: commit.modelData.selected ? Theme.s.background_accent
                             : hover.hovered ? Qt.tint(Theme.s.background_elevated, Qt.rgba(0.5, 0.5, 0.5, 0.12)) : "transparent"
                        HoverHandler { id: hover }
                        TapHandler {
                            onTapped: (point) => {
                                const mods = point.modifiers
                                if (mods & Qt.ShiftModifier && view.anchorRow >= 0) {
                                    Changes.selectRange(view.anchorRow, commit.modelData.row)
                                } else if (mods & Qt.ControlModifier) {
                                    Changes.toggle(commit.modelData.hash)
                                    view.anchorRow = commit.modelData.row
                                } else {
                                    Changes.select(commit.modelData.hash)
                                    view.anchorRow = commit.modelData.row
                                }
                            }
                        }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 8
                            Rectangle {
                                implicitWidth: 10
                                implicitHeight: 10
                                radius: 5
                                color: commit.modelData.selected ? Theme.accent : "transparent"
                                border.color: commit.modelData.workTree ? Theme.changed : Theme.textMuted
                                border.width: 1.5
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    Layout.fillWidth: true
                                    text: commit.modelData.subject
                                    elide: Text.ElideRight
                                    font.italic: commit.modelData.workTree
                                    font.weight: commit.modelData.selected ? Styles.Primitives.fontWeight.semiBold
                                                                           : Styles.Primitives.fontWeight.regular
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: !commit.modelData.workTree
                                    text: commit.modelData.shortHash + "  ·  " + commit.modelData.author + "  ·  " + commit.modelData.date
                                          + (commit.modelData.merge ? qsTr("  ·  merge") : "")
                                    color: Theme.textMuted
                                    font: Theme.smallFont
                                    elide: Text.ElideRight
                                }
                                Label {
                                    visible: commit.modelData.workTree
                                    text: qsTr("staged, unstaged and untracked")
                                    color: Theme.textMuted
                                    font: Theme.smallFont
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- The changed code ---------------------------------------------------
        ColumnLayout {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 300
            spacing: 0
            Pane {
                Layout.fillWidth: true
                padding: Styles.Primitives.spacing.sp075
                background: Rectangle { color: Theme.s.background_default }
                Flow {
                    width: parent.width
                    spacing: Styles.Primitives.spacing.sp100
                    ComboBox {
                        id: contextBox
                        styles: [Styles.FieldStyle.small]
                        width: 230
                        currentIndex: 1
                        model: [qsTr("Changed code only"), qsTr("…and what uses it"), qsTr("…what uses it and what it uses")]
                    }
                    Switch { id: verticalBox; text: qsTr("Top down"); font: Theme.smallFont }
                    Label {
                        height: 28
                        verticalAlignment: Text.AlignVCenter
                        text: Changes.busy ? qsTr("Reading the commits…") : Changes.summary
                        color: Theme.textMuted
                        font: Theme.smallFont
                    }
                    Button {
                        visible: Changes.active
                        styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                        icon.source: Icons.source("copy")
                        text: qsTr("Copy as Markdown")
                        onClicked: Session.copyToClipboard(Changes.markdown())
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Commits, files, symbols and findings of this change, for a review")
                    }
                }
            }
            GraphCanvas {
                id: canvas
                Layout.fillWidth: true
                Layout.fillHeight: true
                scene: scene
                showLegend: false
                onEdgeClicked: edge => {
                    const i = scene.edgeInfo(edge)
                    refs.title = i.fromName + " → " + i.toName
                    refs.references = scene.edgeReferences(edge)
                    refs.visible = true
                }
                Label {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.margins: 10
                    text: scene.summary
                    color: Theme.textMuted
                    font: Theme.smallFont
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    running: Changes.busy
                    visible: running
                }
                ReferenceList {
                    id: refs
                    visible: false
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    width: Math.min(420, parent.width * 0.5)
                    height: Math.min(320, parent.height - 80)
                    onCloseRequested: visible = false
                }
            }
        }

        // --- What is in it ------------------------------------------------------
        Pane {
            SplitView.preferredWidth: 320
            SplitView.minimumWidth: 220
            padding: 0
            background: Rectangle { color: Theme.s.background_elevated }
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                TabBar {
                    id: detailTabs
                    Layout.fillWidth: true
                    Layout.margins: Styles.Primitives.spacing.sp075
                    TabButton { text: qsTr("Symbols (%1)").arg(Changes.symbols.length); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Files (%1)").arg(Changes.files.length); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Findings (%1)").arg(Changes.findings.length); styles: [Styles.TabStyle.small]; width: implicitWidth }
                }
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: detailTabs.currentIndex

                    ListView {
                        clip: true
                        model: Changes.symbols
                        ScrollBar.vertical: ScrollBar {}
                        section.property: "path"
                        section.delegate: Label {
                            required property string section
                            width: ListView.view.width
                            leftPadding: 10
                            topPadding: 8
                            bottomPadding: 2
                            text: section
                            color: Theme.textMuted
                            font: Theme.smallFont
                            elide: Text.ElideMiddle
                        }
                        delegate: ItemDelegate {
                            id: sym
                            required property var modelData
                            width: ListView.view.width
                            styles: [Styles.ItemStyle.listRow, Styles.ItemStyle.small]
                            highlighted: modelData.symbol === Session.activeSymbol
                            onClicked: Session.activate(modelData.symbol)
                            contentItem: RowLayout {
                                spacing: 6
                                Rectangle {
                                    implicitWidth: 3
                                    implicitHeight: 16
                                    radius: 1.5
                                    color: sym.modelData.added ? Theme.added : Theme.changed
                                }
                                Icon { name: Theme.kindIcon(sym.modelData.kind); size: 13; color: Theme.kindColor(sym.modelData.kind) }
                                Label { text: sym.modelData.qualified; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                Label {
                                    text: (sym.modelData.added ? qsTr("new ") : "") + sym.modelData.lines
                                    color: sym.modelData.added ? Theme.added : Theme.textMuted
                                    font: Theme.smallFont
                                }
                            }
                        }
                    }
                    ListView {
                        clip: true
                        model: Changes.files
                        ScrollBar.vertical: ScrollBar {}
                        delegate: ItemDelegate {
                            id: file
                            required property var modelData
                            width: ListView.view.width
                            styles: [Styles.ItemStyle.listRow, Styles.ItemStyle.small]
                            enabled: modelData.indexed
                            onClicked: Session.activateFile(modelData.file, 0)
                            contentItem: RowLayout {
                                spacing: 6
                                Icon { name: "file-code"; size: 13; color: Theme.fileColor }
                                Label { text: file.modelData.relative; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                Label { text: "+" + file.modelData.added; color: Theme.added; font: Theme.smallFont }
                                Label { text: "−" + file.modelData.removed; color: Theme.danger; font: Theme.smallFont }
                            }
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: (modelData.indexed ? qsTr("%n changed symbols", "", modelData.symbols) : qsTr("not indexed"))
                                          + (modelData.newFile ? qsTr(" · new file") : "") + "\n" + modelData.commits
                        }
                    }
                    ListView {
                        clip: true
                        model: Changes.findings
                        ScrollBar.vertical: ScrollBar {}
                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 32
                            visible: parent.count === 0
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                            color: Theme.textMuted
                            text: Changes.active ? qsTr("The review found nothing in the changed code") : ""
                        }
                        delegate: ItemDelegate {
                            id: fd
                            required property var modelData
                            width: ListView.view.width
                            styles: [Styles.ItemStyle.listRow]
                            implicitHeight: fdText.implicitHeight + 12
                            onClicked: modelData.symbol >= 0 ? Session.activate(modelData.symbol) : Session.activateFile(modelData.file, modelData.line)
                            contentItem: RowLayout {
                                spacing: 8
                                Rectangle {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.topMargin: 4
                                    implicitWidth: 8; implicitHeight: 8; radius: 4
                                    color: Theme.severityColor(fd.modelData.severity)
                                }
                                ColumnLayout {
                                    id: fdText
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label { text: fd.modelData.kindName; color: Theme.textMuted; font: Theme.smallFont }
                                    Label { text: fd.modelData.title; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                    Label { text: fd.modelData.detail; color: Theme.textMuted; font: Theme.smallFont; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
