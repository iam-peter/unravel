pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// Fuzzy search for symbols and files, Ctrl+K: type, pick with the arrows,
// Enter shows it everywhere
Item {
    id: box

    implicitHeight: field.implicitHeight
    function focusSearch() {
        field.forceActiveFocus()
        field.selectAll()
    }

    // Searched a moment after the last key, not on every one
    Timer {
        id: debounce
        interval: 90
        onTriggered: results.query = field.text
    }
    SymbolSearch {
        id: results
        category: categories.currentIndex
        includeExternal: externalSwitch.checked
    }

    TextField {
        id: field
        anchors.fill: parent
        placeholderText: qsTr("Search symbols and files  (Ctrl+K)")
        leftPadding: 30
        onTextChanged: {
            debounce.restart()
            list.currentIndex = 0
            if (text.length > 0 && !popup.opened) popup.open()
        }
        onActiveFocusChanged: if (activeFocus && text.length > 0) popup.open()
        Keys.onDownPressed: list.incrementCurrentIndex()
        Keys.onUpPressed: list.decrementCurrentIndex()
        Keys.onPressed: event => {
            if (event.key === Qt.Key_PageDown) { list.currentIndex = Math.min(list.count - 1, list.currentIndex + 10); event.accepted = true }
            else if (event.key === Qt.Key_PageUp) { list.currentIndex = Math.max(0, list.currentIndex - 10); event.accepted = true }
            else if (event.key === Qt.Key_Tab) { categories.currentIndex = (categories.currentIndex + 1) % 6; event.accepted = true }
        }
        Keys.onReturnPressed: box.pickNow()
        Keys.onEnterPressed: box.pickNow()
        Keys.onEscapePressed: { popup.close(); field.focus = false }
        Icon {
            anchors.left: parent.left
            anchors.leftMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            size: 14
            color: Theme.textMuted
        }
    }

    // Enter right after typing: search first
    function pickNow() {
        if (debounce.running) {
            debounce.stop()
            results.query = field.text
        }
        pick(list.currentIndex)
    }
    function pick(row) {
        if (row < 0 || row >= results.count)
            return
        const r = results.get(row)
        if (r.symbol >= 0)
            Session.activate(r.symbol)
        else if (r.file >= 0)
            Session.activateFile(r.file, 0)
        popup.close()
        field.focus = false
    }

    Popup {
        id: popup
        y: field.height + 4
        width: Math.max(box.width, 560)
        x: (box.width - width) / 2
        height: Math.min(520, header.implicitHeight + list.contentHeight + 24)
        padding: 6
        // Not modal, unlike the style's popups: the field stays editable
        // with the mouse while the results show. A press outside the field
        // closes them and goes on to what it was on.
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        ColumnLayout {
            anchors.fill: parent
            spacing: 4
            RowLayout {
                id: header
                Layout.fillWidth: true
                TabBar {
                    id: categories
                    Layout.minimumWidth: implicitWidth
                    TabButton { text: qsTr("All"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Types"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Functions"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Variables"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Files"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Macros"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                }
                Item { Layout.fillWidth: true }
                Switch {
                    id: externalSwitch
                    text: qsTr("Libraries too")
                    font: Theme.smallFont
                }
            }
            Label {
                visible: results.count === 0
                text: field.text.length ? qsTr("Nothing matches “%1”").arg(field.text) : qsTr("Type part of a name; “a::b” searches qualified names")
                color: Theme.textMuted
                Layout.margins: 8
            }
            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: results
                currentIndex: 0
                highlightMoveDuration: 0
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: ItemDelegate {
                    id: row
                    required property int index
                    required property int symbol
                    required property int kind
                    required property string kindName
                    required property string nameHtml
                    required property string qualified
                    required property string location
                    required property bool external
                    width: ListView.view.width
                    styles: [Styles.ItemStyle.listRow, Styles.ItemStyle.medium]
                    highlighted: ListView.isCurrentItem
                    onClicked: box.pick(index)
                    contentItem: RowLayout {
                        spacing: 8
                        Rectangle {
                            implicitWidth: 20
                            implicitHeight: 20
                            radius: 3
                            color: Qt.tint(Theme.card, Styles.Primitives.alpha(Theme.kindColor(row.kind), 0.35))
                            Icon {
                                anchors.centerIn: parent
                                name: Theme.kindIcon(row.kind)
                                size: 13
                                color: Theme.text
                            }
                        }
                        Label {
                            text: row.nameHtml
                            textFormat: Text.StyledText
                            font.weight: Styles.Primitives.fontWeight.medium
                            opacity: row.external ? 0.7 : 1
                        }
                        Label {
                            Layout.fillWidth: true
                            text: row.symbol >= 0 && row.qualified !== row.nameHtml ? row.qualified : ""
                            color: Theme.textMuted
                            elide: Text.ElideMiddle
                            font: Theme.smallFont
                        }
                        Label {
                            text: row.kindName
                            color: Theme.kindColor(row.kind)
                            font: Theme.smallFont
                        }
                        Label {
                            Layout.maximumWidth: 200
                            text: row.location
                            color: Theme.textMuted
                            elide: Text.ElideLeft
                            font: Theme.smallFont
                        }
                    }
                }
            }
        }
    }
}
