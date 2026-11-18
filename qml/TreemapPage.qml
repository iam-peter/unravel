pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// The code as a treemap: folders hold files, files hold their classes and
// functions; area is size, colour a metric. Double-click zooms into a folder.
Item {
    id: view
    property bool active: false

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
                IconButton {
                    iconName: "arrow-up"
                    enabled: map.rootPath !== ""
                    onClicked: {
                        const p = map.rootPath
                        const slash = p.lastIndexOf("/")
                        map.rootPath = slash < 0 ? "" : p.substring(0, slash)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Up a folder")
                }
                Label {
                    text: map.rootPath === "" ? qsTr("Whole project") : map.rootPath
                    font.weight: Styles.Primitives.fontWeight.semiBold
                }
                ToolSeparator {}
                Label { text: qsTr("Size"); color: Theme.textMuted }
                ComboBox {
                    id: sizeBox
                    styles: [Styles.FieldStyle.small]
                    model: [qsTr("Lines"), qsTr("Statements"), qsTr("Same for all")]
                    implicitWidth: 130
                }
                Label { text: qsTr("Colour"); color: Theme.textMuted }
                ComboBox {
                    id: colorBox
                    styles: [Styles.FieldStyle.small]
                    model: [qsTr("Complexity"), qsTr("Callers (fan-in)"), qsTr("Uses (fan-out)"), qsTr("Looks unused"),
                            qsTr("Findings"), qsTr("Nesting"), qsTr("Kind"), qsTr("Git churn (files)"),
                            qsTr("Review priority"), qsTr("Header impact (files)")]
                    implicitWidth: 190
                }
                Item { Layout.fillWidth: true }
                Label { text: map.legendLow; font: Theme.smallFont; color: Theme.textMuted }
                Rectangle {
                    visible: colorBox.currentIndex !== TreemapView.Kind
                             && (colorBox.currentIndex !== TreemapView.Churn || Hotspots.ready)
                    Layout.preferredWidth: 80
                    Layout.minimumWidth: 30
                    Layout.preferredHeight: 8
                    radius: 4
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: Theme.viewColors.low }
                        GradientStop { position: 0.5; color: Theme.viewColors.mid }
                        GradientStop { position: 1; color: Theme.viewColors.high }
                    }
                }
                Label { text: map.legendHigh; font: Theme.smallFont; color: Theme.textMuted }
            }
        }
        HistoryBar {
            Layout.fillWidth: true
            visible: colorBox.currentIndex === TreemapView.Churn || colorBox.currentIndex === TreemapView.Priority
        }
        Label {
            Layout.fillWidth: true
            Layout.margins: 6
            visible: colorBox.currentIndex === TreemapView.Priority || colorBox.currentIndex === TreemapView.HeaderImpact
            text: colorBox.currentIndex === TreemapView.Priority
                  ? qsTr("Heuristic v1; file-history proxy when available, static otherwise. File/folder values are maxima; gray means unavailable. See Review > Focus for evidence.")
                  : qsTr("Distinct indexed translation units, not measured build time. Gray: non-header/unavailable; folder values are maxima. See Review > Header impact for coverage and witnesses.")
            color: Theme.textMuted
            wrapMode: Text.Wrap
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.canvas
            TreemapView {
                id: map
                anchors.fill: parent
                anchors.margins: 4
                colorMetric: colorBox.currentIndex
                sizeMetric: sizeBox.currentIndex
                colors: Theme.viewColors
                activeSymbol: Session.activeSymbol
                HoverHandler {
                    id: hover
                    onPointChanged: map.hover(point.position.x, point.position.y)
                }
                TapHandler {
                    onTapped: point => {
                        const it = map.itemAt(point.position.x, point.position.y)
                        if (it.symbol !== undefined && it.symbol >= 0) Session.activate(it.symbol)
                        else if (it.file !== undefined && it.file >= 0) Session.activateFile(it.file, 0)
                    }
                    onDoubleTapped: point => {
                        const it = map.itemAt(point.position.x, point.position.y)
                        if (it.enclosingFolder !== undefined && it.enclosingFolder !== map.rootPath)
                            map.rootPath = it.enclosingFolder
                    }
                }
                ToolTip {
                    visible: hover.hovered && map.hoveredInfo.name !== undefined
                    x: hover.point.position.x + 16
                    y: hover.point.position.y + 16
                    text: map.hoveredInfo.name === undefined ? "" :
                          map.hoveredInfo.name + "\n" + map.hoveredInfo.kind + " · " + Math.round(map.hoveredInfo.size)
                          + (sizeBox.currentIndex === 0 ? qsTr(" lines") : "")
                          + (colorBox.currentIndex === TreemapView.Churn
                             ? (map.hoveredInfo.value >= 0
                                ? (map.hoveredInfo.file >= 0 ? qsTr(" · %1 commits") : qsTr(" · max %1 commits per file")).arg(map.hoveredInfo.value)
                                : qsTr(" · history unavailable"))
                             : colorBox.currentIndex === TreemapView.Priority || colorBox.currentIndex === TreemapView.HeaderImpact
                               ? (map.hoveredInfo.value >= 0 ? " · " + colorBox.currentText + " " + Math.round(map.hoveredInfo.value * 100) / 100 : qsTr(" · unavailable"))
                             : (map.hoveredInfo.symbol >= 0 && colorBox.currentIndex < TreemapView.Kind
                                ? " · " + colorBox.currentText.toLowerCase() + " " + Math.round(map.hoveredInfo.value * 10) / 10 : ""))
                }
            }
        }
    }
}
