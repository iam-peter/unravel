pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// Sourcetrail's code view: the active symbol's definition, then every
// place that uses it with the lines around, each under what it is in.
// Every name clang knows is a link. Or a whole file.
Pane {
    id: codeView

    padding: 0
    background: Rectangle { color: Theme.codeBackground }

    CodeSnippets {
        id: code
        symbol: Session.activeSymbol
        file: Session.activeFile
        line: Session.activeSymbol < 0 ? Session.activeInfo.line || 0 : 0
        wholeFile: modeBar.currentIndex === 1
        context: Theme.prefs.snippetContext
        colors: Theme.codeColors
    }

    // A reference activates a file at a line: show where
    Connections {
        target: Session
        function onActiveChanged() {
            if (Session.activeSymbol < 0 && Session.activeFile >= 0)
                Qt.callLater(codeView.scrollToFocus)
        }
    }
    function scrollToFocus() {
        if (list.count === 1 && code.snippets.length === 1) {
            const s = code.snippets[0]
            const line = s.focusLine || 0
            const item = list.itemAtIndex(0)
            if (item && line > 0)
                list.contentY = Math.max(0, item.y + 30 + (line - s.from) * item.lineHeight - list.height / 3)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            Layout.fillWidth: true
            padding: Styles.Primitives.spacing.sp075
            background: Rectangle {
                color: Theme.s.background_elevated
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.cardBorder }
            }
            RowLayout {
                width: parent.width
                TabBar {
                    id: modeBar
                    Layout.minimumWidth: implicitWidth
                    enabled: Session.activeSymbol >= 0
                    TabButton { text: qsTr("Uses"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("File"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: Theme.textMuted
                    font: Theme.smallFont
                    text: Session.activeSymbol >= 0 && modeBar.currentIndex === 0
                          ? qsTr("%n references", "", code.referenceCount) + qsTr(" in %n files", "", code.fileCount)
                            + (code.hiddenCount > 0 ? qsTr(" · %1 not shown").arg(code.hiddenCount) : "")
                          : ""
                }
                Label { text: qsTr("Context"); color: Theme.textMuted; font: Theme.smallFont; visible: modeBar.currentIndex === 0 }
                SpinBox {
                    visible: modeBar.currentIndex === 0
                    from: 0; to: 12
                    value: Theme.prefs.snippetContext
                    onValueModified: Theme.prefs.snippetContext = value
                    styles: [Styles.FieldStyle.small]
                    implicitWidth: 90
                }
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: code.snippets
            spacing: 8
            topMargin: 8
            bottomMargin: 8
            cacheBuffer: 2000
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            onModelChanged: { contentY = -topMargin; Qt.callLater(codeView.scrollToFocus) }
            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: qsTr("No code to show")
                color: Theme.textMuted
            }
            delegate: Rectangle {
                id: snip
                required property var modelData
                required property int index
                readonly property real lineHeight: text.lineCount > 0 ? text.contentHeight / text.lineCount : 16
                width: ListView.view.width - 16
                x: 8
                height: head.height + text.height + 10
                radius: Theme.radius
                color: Theme.card
                border.color: modelData.definition ? Styles.Primitives.alpha(Theme.accent, 0.6) : Theme.cardBorder

                Rectangle {
                    id: head
                    width: parent.width
                    height: 28
                    radius: Theme.radius
                    color: Styles.Primitives.alpha(Theme.s.background_muted, 0.7)
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 4
                        spacing: 6
                        Icon { name: "file-code"; size: 13; color: Theme.fileColor }
                        Label {
                            text: snip.modelData.name
                            font.pixelSize: Styles.Primitives.fontSize.small
                            font.weight: Styles.Primitives.fontWeight.semiBold
                            TapHandler { onTapped: Session.activateFile(snip.modelData.file, snip.modelData.from) }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            ToolTip.visible: nameHover.hovered
                            ToolTip.text: snip.modelData.path
                            HoverHandler { id: nameHover }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: (snip.modelData.title || "") + (snip.modelData.kinds ? "  ·  " + snip.modelData.kinds : "")
                            color: snip.modelData.definition ? Theme.accent : Theme.textMuted
                            font: Theme.smallFont
                            elide: Text.ElideMiddle
                            TapHandler {
                                onTapped: if (snip.modelData.container !== undefined && snip.modelData.container >= 0)
                                              Session.activate(snip.modelData.container)
                            }
                        }
                        Label {
                            text: snip.modelData.from === snip.modelData.to ? ":" + snip.modelData.from : ":" + snip.modelData.from + "–" + snip.modelData.to
                            color: Theme.textMuted
                            font: Theme.smallFont
                        }
                        IconButton {
                            iconName: "external-link"
                            onClicked: Session.openInEditor(snip.modelData.file, snip.modelData.from + Math.floor((snip.modelData.to - snip.modelData.from) / 2))
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Open in the editor")
                        }
                    }
                }
                TextEdit {
                    id: text
                    anchors.top: head.bottom
                    anchors.topMargin: 4
                    x: 8
                    width: parent.width - 16
                    readOnly: true
                    selectByMouse: true
                    textFormat: TextEdit.RichText
                    text: snip.modelData.html
                    font: Theme.monoFont
                    color: Theme.text
                    wrapMode: TextEdit.NoWrap
                    selectionColor: Theme.s.selection_default
                    onLinkActivated: link => {
                        if (link.startsWith("s:"))
                            Session.activate(parseInt(link.substring(2)))
                    }
                    HoverHandler {
                        cursorShape: text.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.IBeamCursor
                    }
                    ToolTip.visible: hoveredLink !== ""
                    ToolTip.delay: 500
                    ToolTip.text: {
                        if (!hoveredLink.startsWith("s:")) return ""
                        const i = Session.symbolInfo(parseInt(hoveredLink.substring(2)))
                        return (i.signature || i.qualified) + "\n" + i.kindName + (i.location ? " · " + i.location : "")
                    }
                }
                Label {
                    visible: (snip.modelData.truncated || 0) > 0
                    anchors.bottom: parent.bottom
                    anchors.right: parent.right
                    anchors.margins: 6
                    text: qsTr("%n more lines: open the file", "", snip.modelData.truncated || 0)
                    color: Theme.link
                    font: Theme.smallFont
                    TapHandler { onTapped: modeBar.currentIndex = 1 }
                }
            }
        }
    }
}
