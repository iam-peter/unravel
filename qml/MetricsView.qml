pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import IgittStyle as Styles
import Unravel

// Every function, class, file or module with its measurements, as a table
// lens: each number with a bar for its share of the column's largest,
// values past the review thresholds in warning colours
Item {
    id: view
    property bool active: false

    MetricsTable {
        id: table
        mode: modeBar.currentIndex
        filter: filterField.text
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
                TabBar {
                    id: modeBar
                    Layout.minimumWidth: implicitWidth
                    TabButton { text: qsTr("Functions"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Classes"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Files"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                    TabButton { text: qsTr("Modules"); styles: [Styles.TabStyle.small]; width: implicitWidth }
                }
                TextField {
                    id: filterField
                    styles: [Styles.FieldStyle.small]
                    placeholderText: qsTr("Filter")
                    Layout.preferredWidth: 220
                }
                Label { text: qsTr("%n rows", "", table.count); color: Theme.textMuted; font: Theme.smallFont }
                Item { Layout.fillWidth: true }
                Button {
                    styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                    icon.source: Icons.source("copy")
                    text: qsTr("Copy as CSV")
                    onClicked: Session.copyToClipboard(table.toCsv())
                }
            }
        }

        HorizontalHeaderView {
            id: header
            Layout.fillWidth: true
            syncView: grid
            clip: true
            delegate: Rectangle {
                id: head
                required property int index
                required property string display
                implicitHeight: 30
                implicitWidth: 80
                color: Theme.s.background_elevated
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.cardBorder }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 6
                    Label {
                        Layout.fillWidth: true
                        text: head.display
                        font.weight: Styles.Primitives.fontWeight.semiBold
                        elide: Text.ElideRight
                        horizontalAlignment: head.index === 0 || head.index === table.headers.length - 1 ? Text.AlignLeft : Text.AlignRight
                    }
                    Icon {
                        visible: table.sortColumn === head.index
                        name: table.descending ? "arrow-down" : "arrow-up"
                        size: 12
                        color: Theme.textMuted
                    }
                }
                TapHandler { onTapped: table.sortBy(head.index) }
            }
        }

        TableView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: table
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}
            columnWidthProvider: column => {
                const w = table.columnWidths[column]
                if (column === 0) return Math.max(w, grid.width - table.columnWidths.reduce((a, b) => a + b, 0) + w)
                return w
            }
            Connections { target: table; function onModeChanged() { grid.forceLayout() } }
            delegate: Rectangle {
                id: cell
                required property int row
                required property int column
                required property string display
                required property real fraction
                required property bool numeric
                required property bool hot
                required property int symbol
                required property int file
                implicitHeight: 26
                color: (table.rowInfo(row).symbol === Session.activeSymbol && Session.activeSymbol >= 0) ? Theme.s.background_accent
                     : row % 2 ? Theme.s.background_stripe : Theme.s.background_elevated
                Rectangle {
                    visible: cell.numeric && cell.fraction > 0
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height - 8
                    width: Math.max(1, (parent.width - 8) * cell.fraction)
                    radius: 2
                    color: cell.hot ? Styles.Primitives.alpha(Theme.danger, 0.28) : Styles.Primitives.alpha(Theme.accent, 0.16)
                }
                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: cell.numeric ? Text.AlignRight : Text.AlignLeft
                    text: cell.display
                    elide: cell.column === 0 ? Text.ElideMiddle : Text.ElideLeft
                    font.family: cell.numeric || cell.column === 0 ? Theme.monoFont.family : Theme.uiFont.family
                    font.pointSize: Theme.monoFont.pointSize
                    color: cell.hot ? Theme.danger : cell.column > 0 && !cell.numeric ? Theme.textMuted : Theme.text
                    font.weight: cell.hot ? Styles.Primitives.fontWeight.semiBold : Styles.Primitives.fontWeight.regular
                }
                TapHandler {
                    onTapped: {
                        const r = table.rowInfo(cell.row)
                        if (r.symbol >= 0) Session.activate(r.symbol)
                        else if (r.file >= 0) Session.activateFile(r.file, 0)
                    }
                }
            }
        }
    }
}
