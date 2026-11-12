pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// The review: what the analysis found worth a look, by kind, with why it
// matters; each finding leads to its code. Exports as a Markdown report.
Item {
    id: view
    property bool active: false
    signal showView(string id)

    FindingsList {
        id: findings
        kind: kindList.currentIndex <= 0 ? -1 : kindList.model[kindList.currentIndex - 1].kind
        minSeverity: severityBox.currentIndex
        filter: filterField.text
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The project at a glance
        Pane {
            Layout.fillWidth: true
            padding: Styles.Primitives.spacing.sp150
            background: Rectangle { color: Theme.s.background_default }
            RowLayout {
                width: parent.width
                spacing: Styles.Primitives.spacing.sp150
                Flow {
                Layout.fillWidth: true
                spacing: Styles.Primitives.spacing.sp100
                Repeater {
                    model: [
                        { label: qsTr("Serious"), value: Session.stats.serious, color: Theme.danger },
                        { label: qsTr("Warnings"), value: Session.stats.findingWarnings, color: Theme.warning },
                        { label: qsTr("Notes"), value: Session.stats.findingNotes, color: Theme.textMuted },
                        { label: qsTr("Files"), value: Session.stats.files, color: Theme.text },
                        { label: qsTr("Lines of code"), value: Session.stats.codeLines, color: Theme.text },
                        { label: qsTr("Functions"), value: Session.stats.functions, color: Theme.text },
                        { label: qsTr("Compiler errors"), value: Session.stats.errors, color: Session.stats.errors > 0 ? Theme.danger : Theme.text },
                    ]
                    Rectangle {
                        required property var modelData
                        width: 118
                        height: 54
                        radius: Theme.radius
                        color: Theme.card
                        border.color: Theme.cardBorder
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 0
                            Label {
                                text: Number(parent.parent.modelData.value || 0).toLocaleString(Qt.locale(), "f", 0)
                                font.pixelSize: 20
                                font.weight: Styles.Primitives.fontWeight.semiBold
                                color: parent.parent.modelData.color
                            }
                            Label { text: parent.parent.modelData.label; color: Theme.textMuted; font: Theme.smallFont }
                        }
                    }
                }
                }
                ColumnLayout {
                    Layout.alignment: Qt.AlignTop
                    Button {
                        styles: [Styles.ButtonStyle.outline, Styles.ButtonStyle.small]
                        icon.source: Icons.source("copy")
                        text: qsTr("Copy report")
                        onClicked: Session.copyToClipboard(Session.reportMarkdown())
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("The whole review as Markdown: for a pull request, a ticket or an assistant")
                    }
                }
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            handle: SplitterHandle {}

            ListView {
                id: kindList
                SplitView.preferredWidth: 260
                SplitView.minimumWidth: 200
                clip: true
                model: findings.kinds
                currentIndex: 0
                header: ItemDelegate {
                    width: ListView.view.width
                    styles: [Styles.ItemStyle.listRow]
                    highlighted: kindList.currentIndex <= 0
                    text: qsTr("Everything (%1)").arg(Session.stats.findings || 0)
                    icon.source: Icons.source("list-checks")
                    onClicked: kindList.currentIndex = 0
                }
                delegate: ItemDelegate {
                    id: kd
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    styles: [Styles.ItemStyle.listRow]
                    highlighted: kindList.currentIndex === index + 1
                    onClicked: kindList.currentIndex = index + 1
                    contentItem: RowLayout {
                        spacing: 6
                        Rectangle {
                            implicitWidth: 8; implicitHeight: 8; radius: 4
                            color: kd.modelData.serious > 0 ? Theme.danger : Theme.warning
                        }
                        Label { text: kd.modelData.name; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label { text: kd.modelData.count; color: Theme.textMuted }
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: modelData.hint
                }
            }

            ColumnLayout {
                SplitView.fillWidth: true
                spacing: 0
                Pane {
                    Layout.fillWidth: true
                    padding: Styles.Primitives.spacing.sp100
                    background: Rectangle { color: Theme.s.background_elevated }
                    ColumnLayout {
                        width: parent.width
                        RowLayout {
                            Layout.fillWidth: true
                            TextField {
                                id: filterField
                                styles: [Styles.FieldStyle.small]
                                placeholderText: qsTr("Filter")
                                Layout.preferredWidth: 220
                            }
                            ComboBox {
                                id: severityBox
                                styles: [Styles.FieldStyle.small]
                                model: [qsTr("Everything"), qsTr("Warnings and worse"), qsTr("Serious only")]
                                implicitWidth: 180
                            }
                            Item { Layout.fillWidth: true }
                            Label { text: qsTr("%n findings", "", findings.count); color: Theme.textMuted }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: findings.kind >= 0
                            wrapMode: Text.Wrap
                            color: Theme.textMuted
                            text: findings.kind >= 0 ? findings.hint(findings.kind) : ""
                        }
                    }
                }
                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: findings
                    ScrollBar.vertical: ScrollBar {}
                    section.property: "kindName"
                    section.criteria: ViewSection.FullString
                    section.delegate: Label {
                        required property string section
                        width: ListView.view.width
                        leftPadding: 12
                        topPadding: 12
                        bottomPadding: 4
                        text: section
                        font.weight: Styles.Primitives.fontWeight.semiBold
                        color: Theme.textMuted
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: list.count === 0
                        text: qsTr("Nothing to report here")
                        color: Theme.textMuted
                    }
                    delegate: ItemDelegate {
                        id: fd
                        required property int severity
                        required property string title
                        required property string detail
                        required property string location
                        required property int symbol
                        required property int file
                        required property int line
                        required property var symbols
                        required property int kind
                        width: ListView.view.width
                        styles: [Styles.ItemStyle.listRow]
                        highlighted: symbol >= 0 && symbol === Session.activeSymbol
                        implicitHeight: content.implicitHeight + 14
                        onClicked: {
                            if (symbol >= 0) Session.activate(symbol)
                            else if (file >= 0) Session.activateFile(file, line)
                        }
                        contentItem: RowLayout {
                            id: content
                            spacing: 10
                            Rectangle {
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: 5
                                implicitWidth: 9; implicitHeight: 9; radius: 4.5
                                color: Theme.severityColor(fd.severity)
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label { text: fd.title; font.weight: Styles.Primitives.fontWeight.medium; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                Label { text: fd.detail; color: Theme.textMuted; wrapMode: Text.Wrap; Layout.fillWidth: true; font: Theme.smallFont }
                                // Clone groups: each member
                                Flow {
                                    Layout.fillWidth: true
                                    visible: fd.kind === 10 && fd.symbols.length > 1
                                    spacing: 6
                                    Repeater {
                                        model: fd.kind === 10 ? fd.symbols : []
                                        Button {
                                            required property int modelData
                                            styles: [Styles.ButtonStyle.outline, Styles.ButtonStyle.small]
                                            text: Session.symbolInfo(modelData).qualified
                                            onClicked: Session.activate(modelData)
                                            font: Theme.smallFont
                                        }
                                    }
                                }
                            }
                            Label {
                                Layout.alignment: Qt.AlignTop
                                text: fd.location
                                color: Theme.link
                                font: Theme.smallFont
                                Layout.maximumWidth: 260
                                elide: Text.ElideLeft
                                TapHandler { onTapped: Session.openInEditor(fd.file, fd.line) }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                            }
                        }
                    }
                }
            }
        }
    }
}
