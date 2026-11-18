pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// A zoomable, pannable canvas for a GraphScene: nodes as cards, edges
// painted beneath them. Clicking a node or a member activates it; an
// edge shows the references behind it.
Item {
    id: canvas

    required property GraphScene scene
    property real zoom: 1
    property real panX: 0
    property real panY: 0
    property bool autoFit: true
    property int hoveredEdge: -1
    property bool showLegend: Theme.prefs.showLegend
    signal edgeClicked(int edge)
    signal nodeActivated(var node)

    clip: true

    function clampZoom(z) { return Math.max(0.15, Math.min(3, z)) }
    function zoomAt(px, py, z) {
        z = clampZoom(z)
        const wx = (px - panX) / zoom, wy = (py - panY) / zoom
        zoom = z
        panX = px - wx * zoom
        panY = py - wy * zoom
    }
    function zoomBy(f) { zoomAt(width / 2, height / 2, zoom * f) }
    function fit(maxZoom) {
        if (!scene || scene.sceneWidth <= 0 || width <= 0)
            return
        const z = clampZoom(Math.min((width - 40) / scene.sceneWidth, (height - 40) / scene.sceneHeight, maxZoom || 1))
        zoom = z
        panX = (width - scene.sceneWidth * z) / 2
        panY = (height - scene.sceneHeight * z) / 2
    }
    // Whether the user zoomed or panned since the last arrange: then a
    // resize leaves the view alone
    property bool userMoved: false
    // The scene is laid out again: fit it, but not smaller than readable;
    // when it's bigger, keep the centre node in view
    function arrange() {
        if (!scene || scene.sceneWidth <= 0 || width <= 0 || height <= 0 || !visible)
            return
        userMoved = false
        const fitZoom = Math.min((width - 40) / scene.sceneWidth, (height - 40) / scene.sceneHeight, 1)
        if (fitZoom >= 0.7 || scene.nodes.length === 0) {
            fit(1)
            return
        }
        zoom = 0.7
        let center = 0
        for (let i = 0; i < scene.nodes.length; ++i)
            if (scene.nodes[i].center) { center = i; break }
        const r = scene.nodeRect(center)
        const sw = scene.sceneWidth * zoom, sh = scene.sceneHeight * zoom
        let px = width / 2 - (r.x + r.width / 2) * zoom
        let py = height / 2 - (r.y + r.height / 2) * zoom
        // As much of the scene as fits, without empty margins
        px = sw <= width ? (width - sw) / 2 : Math.min(0, Math.max(width - sw, px))
        py = sh <= height ? (height - sh) / 2 : Math.min(0, Math.max(height - sh, py))
        panX = px
        panY = py
        userMoved = false
    }
    Connections {
        target: canvas.scene
        function onChanged() { if (canvas.autoFit) Qt.callLater(canvas.arrange) }
    }
    onWidthChanged: if (autoFit && !userMoved) Qt.callLater(arrange)
    onHeightChanged: if (autoFit && !userMoved) Qt.callLater(arrange)
    onVisibleChanged: if (visible && autoFit && !userMoved) Qt.callLater(arrange)

    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
    }
    GridBackground {
        anchors.fill: parent
        visible: Theme.prefs.showGrid
        offsetX: canvas.panX
        offsetY: canvas.panY
        zoom: canvas.zoom
        color: Theme.gridDot
    }

    // Panning and zooming
    DragHandler {
        id: panDrag
        target: null
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        property real startX
        property real startY
        onActiveChanged: if (active) { startX = canvas.panX; startY = canvas.panY }
        onTranslationChanged: if (active) { canvas.userMoved = true; canvas.panX = startX + translation.x; canvas.panY = startY + translation.y }
        cursorShape: active ? Qt.ClosedHandCursor : Qt.ArrowCursor
    }
    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            canvas.userMoved = true
            const zooms = Theme.prefs.wheelZooms || (event.modifiers & Qt.ControlModifier)
            if (zooms) {
                const delta = event.angleDelta.y / 120
                canvas.zoomAt(event.x, event.y, canvas.zoom * Math.pow(1.15, delta))
            } else {
                canvas.panX += event.pixelDelta.x !== 0 ? event.pixelDelta.x : event.angleDelta.x / 2
                canvas.panY += event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.angleDelta.y / 2
            }
        }
    }
    PinchHandler {
        target: null
        property real startZoom: 1
        onActiveChanged: if (active) startZoom = canvas.zoom
        onActiveScaleChanged: canvas.userMoved = true, canvas.zoomAt(centroid.position.x, centroid.position.y, startZoom * activeScale)
    }
    HoverHandler {
        id: hover
        onPointChanged: {
            const e = edges.edgeAt(point.position.x, point.position.y)
            canvas.hoveredEdge = e
        }
    }
    TapHandler {
        onTapped: (point, button) => {
            const e = edges.edgeAt(point.position.x, point.position.y)
            if (e >= 0) {
                canvas.scene.selectedEdge = e
                canvas.edgeClicked(e)
            } else {
                canvas.scene.selectedEdge = -1
            }
        }
    }

    Binding {
        target: canvas.scene
        property: "edgeStyle"
        value: Theme.prefs.edgeStyle
        when: canvas.scene !== null
    }

    EdgeLayer {
        id: edges
        anchors.fill: parent
        scene: canvas.scene
        zoom: canvas.zoom
        panX: canvas.panX
        panY: canvas.panY
        colors: Theme.edgeColors
        hoveredEdge: canvas.hoveredEdge
    }

    Item {
        id: content
        x: canvas.panX
        y: canvas.panY
        scale: canvas.zoom
        transformOrigin: Item.TopLeft

        Repeater {
            model: canvas.scene ? canvas.scene.groups : []
            Rectangle {
                required property var modelData
                x: modelData.x
                y: modelData.y
                width: modelData.w
                height: modelData.h
                radius: 6
                color: Styles.Primitives.alpha(Theme.namespaceColor, 0.05)
                border.color: Styles.Primitives.alpha(Theme.namespaceColor, 0.35)
                Label {
                    x: 8
                    y: 4
                    text: parent.modelData.label
                    font: Theme.smallFont
                    color: Theme.namespaceColor
                    width: parent.width - 16
                    elide: Text.ElideMiddle
                }
            }
        }
        Repeater {
            model: canvas.scene ? canvas.scene.nodes : []
            NodeCard {
                scene: canvas.scene
                onActivated: node => canvas.nodeActivated(node)
            }
        }
    }

    // The edge under the pointer
    ToolTip {
        visible: canvas.hoveredEdge >= 0 && hover.hovered
        x: hover.point.position.x + 14
        y: hover.point.position.y + 14
        text: {
            if (canvas.hoveredEdge < 0 || !canvas.scene) return ""
            const i = canvas.scene.edgeInfo(canvas.hoveredEdge)
            return qsTr("%1 → %2\n%3 × %4%5").arg(i.fromName).arg(i.toName).arg(i.count).arg(i.kindName)
                     .arg(i.cyclic ? qsTr("  (in a cycle)") : "")
                     + (i.forbidden ? qsTr("\nArchitecture rule violation (orange). See Review > Architecture rules.") : "")
                     + qsTr("\nClick for the references")
        }
    }

    Label {
        anchors.centerIn: parent
        visible: canvas.scene && canvas.scene.nodes.length === 0
        text: canvas.scene && canvas.scene.summary !== "" ? canvas.scene.summary : qsTr("Nothing to show")
        color: Theme.textMuted
    }

    Legend {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 10
        visible: canvas.showLegend
    }

    // Zoom controls
    Pane {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 10
        padding: 2
        background: Rectangle {
            color: Styles.Primitives.alpha(Theme.card, 0.94)
            border.color: Theme.cardBorder
            radius: Theme.radius
        }
        RowLayout {
            spacing: 0
            IconButton { iconName: "zoom-out"; onClicked: canvas.zoomBy(0.8); ToolTip.visible: hovered; ToolTip.text: qsTr("Zoom out") }
            ToolButton {
                styles: [Styles.ButtonStyle.small]
                text: Math.round(canvas.zoom * 100) + "%"
                font: Theme.smallFont
                onClicked: canvas.zoomAt(canvas.width / 2, canvas.height / 2, 1)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Zoom to 100%")
            }
            IconButton { iconName: "zoom-in"; onClicked: canvas.zoomBy(1.25); ToolTip.visible: hovered; ToolTip.text: qsTr("Zoom in") }
            IconButton { iconName: "maximize"; onClicked: canvas.fit(1.5); ToolTip.visible: hovered; ToolTip.text: qsTr("Fit everything") }
            IconButton {
                iconName: Theme.prefs.edgeStyle === 0 ? "spline" : Theme.prefs.edgeStyle === 1 ? "minus" : "route"
                onClicked: Theme.prefs.edgeStyle = (Theme.prefs.edgeStyle + 1) % 3
                ToolTip.visible: hovered
                ToolTip.text: [qsTr("Edges: curved"), qsTr("Edges: straight"), qsTr("Edges: right angles")][Theme.prefs.edgeStyle]
                              + qsTr(" (click for the next; also in the settings)")
            }
            IconButton {
                iconName: "info"
                checkable: true
                checked: canvas.showLegend
                onClicked: Theme.prefs.showLegend = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Legend")
            }
        }
    }
}
