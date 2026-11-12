import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCore
import IgittStyle as Styles
import Unravel

ApplicationWindow {
    id: win

    property string startView: ""
    property string startSymbol: ""

    width: 1560
    height: 940
    minimumWidth: 900
    minimumHeight: 560
    visible: true
    title: (Session.hasModel ? Session.projectName + " — " : "") + "Unravel"
    font: Theme.uiFont

    readonly property var views: [
        { id: "graph", name: qsTr("Graph"), icon: "network", tip: qsTr("The active symbol with what uses it and what it uses") },
        { id: "flow", name: qsTr("Flow"), icon: "workflow", tip: qsTr("Call trees and call paths") },
        { id: "architecture", name: qsTr("Architecture"), icon: "layers", tip: qsTr("Modules, files or classes and their dependencies") },
        { id: "matrix", name: qsTr("Matrix"), icon: "grid-3x3", tip: qsTr("Dependency structure matrix") },
        { id: "bundles", name: qsTr("Bundles"), icon: "orbit", tip: qsTr("Every dependency at once, bundled along the hierarchy") },
        { id: "treemap", name: qsTr("Treemap"), icon: "layout-grid", tip: qsTr("The code by size, coloured by a metric") },
        { id: "metrics", name: qsTr("Metrics"), icon: "table", tip: qsTr("Every function, class, file and module measured") },
        { id: "findings", name: qsTr("Review"), icon: "scan-search", tip: qsTr("What deserves a reviewer's attention") },
    ]
    property int currentView: 0
    function showView(id) {
        for (let i = 0; i < views.length; ++i)
            if (views[i].id === id) currentView = i
    }

    Settings {
        category: "window"
        property alias width: win.width
        property alias height: win.height
        property alias sidebarVisible: sidebar.visible
        property alias codeVisible: rightSplit.visible
        property alias currentView: win.currentView
        property var splitState
        property var rightState
        Component.onCompleted: {
            if (splitState) split.restoreState(splitState)
            if (rightState) rightSplit.restoreState(rightState)
        }
        Component.onDestruction: {
            splitState = split.saveState()
            rightState = rightSplit.saveState()
        }
    }

    readonly property font uiFont: Theme.uiFont
    onUiFontChanged: Fonts.setApplicationFont(uiFont)
    Component.onCompleted: {
        Fonts.setApplicationFont(uiFont)
        if (startView !== "")
            showView(startView)
    }

    Connections {
        target: Session
        function onModelChanged() {
            if (win.startSymbol !== "") {
                const id = Session.symbolByName(win.startSymbol)
                if (id >= 0) Session.activate(id)
                win.startSymbol = ""
            }
        }
        function onMessage(text) { toast.show(text) }
    }

    readonly property bool popupOpen: Overlay.overlay ? Overlay.overlay.children.length > 0 : false

    // --- Shortcuts ----------------------------------------------------------

    Shortcut { sequences: ["Ctrl+K", "Ctrl+F", "Ctrl+P"]; onActivated: search.focusSearch() }
    Shortcut { sequences: ["Alt+Left", "Back", "Ctrl+["]; onActivated: Session.back() }
    Shortcut { sequences: ["Alt+Right", "Forward", "Ctrl+]"]; onActivated: Session.forward() }
    Shortcut { sequences: ["Ctrl+O"]; onActivated: folderDialog.open() }
    Shortcut { sequences: ["F5", "Ctrl+R"]; enabled: Session.hasModel; onActivated: Session.reindex() }
    Shortcut { sequences: ["Ctrl+B"]; onActivated: sidebar.visible = !sidebar.visible }
    Shortcut { sequences: ["Ctrl+J"]; onActivated: rightSplit.visible = !rightSplit.visible }
    Shortcut { sequences: ["Ctrl+D"]; enabled: Session.activeSymbol >= 0; onActivated: Session.toggleBookmark() }
    Shortcut { sequences: ["Ctrl+E"]; enabled: Session.hasModel; onActivated: {
        const i = Session.activeInfo
        if (i.file !== undefined) Session.openInEditor(i.file, i.line || 1)
    } }
    Shortcut { sequences: [StandardKey.Preferences, "Ctrl+,"]; onActivated: settingsDialog.open() }
    Shortcut { sequences: ["F1"]; onActivated: shortcutsDialog.open() }
    Shortcut { sequences: ["Ctrl+1"]; onActivated: win.currentView = 0 }
    Shortcut { sequences: ["Ctrl+2"]; onActivated: win.currentView = 1 }
    Shortcut { sequences: ["Ctrl+3"]; onActivated: win.currentView = 2 }
    Shortcut { sequences: ["Ctrl+4"]; onActivated: win.currentView = 3 }
    Shortcut { sequences: ["Ctrl+5"]; onActivated: win.currentView = 4 }
    Shortcut { sequences: ["Ctrl+6"]; onActivated: win.currentView = 5 }
    Shortcut { sequences: ["Ctrl+7"]; onActivated: win.currentView = 6 }
    Shortcut { sequences: ["Ctrl+8"]; onActivated: win.currentView = 7 }

    // --- Tool bar -------------------------------------------------------------

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            spacing: 2

            IconButton {
                styles: [Styles.ButtonStyle.medium]
                iconName: "panel-left"
                checkable: true
                checked: sidebar.visible
                enabled: Session.hasModel
                onClicked: sidebar.visible = checked
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Show or hide the project (Ctrl+B)")
            }
            IconButton {
                styles: [Styles.ButtonStyle.medium]
                iconName: "arrow-left"
                enabled: Session.canGoBack
                onClicked: Session.back()
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Back (Alt+Left)")
            }
            IconButton {
                styles: [Styles.ButtonStyle.medium]
                iconName: "arrow-right"
                enabled: Session.canGoForward
                onClicked: Session.forward()
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Forward (Alt+Right)")
            }
            IconButton {
                styles: [Styles.ButtonStyle.medium]
                iconName: "history"
                enabled: Session.canGoBack
                onClicked: historyMenu.popup()
                ToolTip.visible: hovered && !historyMenu.visible
                ToolTip.delay: 500
                ToolTip.text: qsTr("Recently active symbols")
                Menu {
                    id: historyMenu
                    Instantiator {
                        model: historyMenu.visible ? Session.history() : []
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.name
                            onTriggered: modelData.symbol >= 0 ? Session.activate(modelData.symbol)
                                                               : Session.activateFile(modelData.file, 0)
                        }
                        onObjectAdded: (index, object) => historyMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => historyMenu.removeItem(object)
                    }
                }
            }

            ToolSeparator {}

            Button {
                id: projectButton
                styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.medium]
                text: Session.hasModel ? Session.projectName : qsTr("Open…")
                font.weight: Styles.Primitives.fontWeight.semiBold
                icon.source: Icons.source("chevron-down")
                icon.width: 14
                icon.height: 14
                LayoutMirroring.enabled: true
                onClicked: projectMenu.popup(projectButton, 0, projectButton.height)
                Menu {
                    id: projectMenu
                    MenuItem { text: qsTr("Open folder…  (Ctrl+O)"); onTriggered: folderDialog.open() }
                    MenuItem { text: qsTr("Open compile_commands.json…"); onTriggered: databaseDialog.open() }
                    Menu {
                        id: recentMenu
                        title: qsTr("Recent")
                        enabled: Session.recentProjects.length > 0
                        Instantiator {
                            model: Session.recentProjects
                            delegate: MenuItem {
                                required property string modelData
                                text: modelData
                                onTriggered: Session.open(modelData)
                            }
                            onObjectAdded: (index, object) => recentMenu.insertItem(index, object)
                            onObjectRemoved: (index, object) => recentMenu.removeItem(object)
                        }
                    }
                    MenuSeparator {}
                    MenuItem { text: qsTr("Index again  (F5)"); enabled: Session.hasModel && Session.state !== Session.Indexing; onTriggered: Session.reindex() }
                    MenuItem { text: qsTr("Compiler errors and warnings…"); enabled: Session.hasModel; onTriggered: errorsDialog.open() }
                    MenuItem { text: qsTr("Open project folder"); enabled: Session.hasModel; onTriggered: Qt.openUrlExternally("file://" + Session.root) }
                    MenuSeparator {}
                    MenuItem { text: qsTr("Copy review report (Markdown)"); enabled: Session.hasModel; onTriggered: { Session.copyToClipboard(Session.reportMarkdown()); toast.show(qsTr("The review report is on the clipboard")) } }
                    MenuItem { text: qsTr("Save review report…"); enabled: Session.hasModel; onTriggered: reportDialog.open() }
                    MenuSeparator {}
                    MenuItem { text: qsTr("Close project"); enabled: Session.hasModel; onTriggered: Session.close() }
                }
            }

            Item { Layout.fillWidth: true; Layout.minimumWidth: 8 }

            SearchBox {
                id: search
                Layout.preferredWidth: Math.min(560, win.width * 0.34)
                enabled: Session.hasModel
            }

            Item { Layout.fillWidth: true; Layout.minimumWidth: 8 }

            IconButton {
                styles: [Styles.ButtonStyle.medium]
                iconName: "star"
                enabled: Session.activeSymbol >= 0
                checkable: true
                checked: Session.activeBookmarked
                onClicked: Session.toggleBookmark()
                icon.color: Session.activeBookmarked ? Theme.warning : palette.buttonText
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: Session.activeBookmarked ? qsTr("Remove the bookmark (Ctrl+D)") : qsTr("Bookmark the active symbol (Ctrl+D)")
            }
            IconButton {
                styles: [Styles.ButtonStyle.medium]
                iconName: "panel-right"
                checkable: true
                checked: rightSplit.visible
                enabled: Session.hasModel
                onClicked: rightSplit.visible = checked
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Show or hide the details and the code (Ctrl+J)")
            }
            IconButton {
                styles: [Styles.ButtonStyle.medium]
                iconName: "settings"
                onClicked: settingsDialog.open()
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Settings (Ctrl+,)")
            }
        }
    }

    // --- Body -------------------------------------------------------------------

    StartPage {
        anchors.fill: parent
        visible: !Session.hasModel
        onOpenFolder: folderDialog.open()
        onOpenDatabase: databaseDialog.open()
    }

    SplitView {
        id: split
        anchors.fill: parent
        visible: Session.hasModel
        orientation: Qt.Horizontal
        handle: SplitterHandle {}

        Sidebar {
            id: sidebar
            SplitView.preferredWidth: 280
            SplitView.minimumWidth: 200
            SplitView.maximumWidth: 520
        }

        ColumnLayout {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 360
            spacing: 0

            ViewBar {
                Layout.fillWidth: true
                views: win.views
                currentIndex: win.currentView
                onActivated: index => win.currentView = index
            }

            StackLayout {
                id: stack
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: win.currentView

                GraphView { active: StackLayout.isCurrentItem }
                FlowView { active: StackLayout.isCurrentItem }
                ArchitectureView { active: StackLayout.isCurrentItem }
                MatrixPage { active: StackLayout.isCurrentItem }
                BundlesPage { active: StackLayout.isCurrentItem }
                TreemapPage { active: StackLayout.isCurrentItem }
                MetricsView { active: StackLayout.isCurrentItem }
                FindingsView { active: StackLayout.isCurrentItem; onShowView: id => win.showView(id) }
            }
        }

        SplitView {
            id: rightSplit
            orientation: Qt.Vertical
            handle: SplitterHandle {}
            SplitView.preferredWidth: 520
            SplitView.minimumWidth: 300

            InfoPanel {
                SplitView.preferredHeight: 230
                SplitView.minimumHeight: 120
                onShowView: id => win.showView(id)
            }
            CodeView {
                SplitView.fillHeight: true
                SplitView.minimumHeight: 160
            }
        }
    }

    // --- Status bar -------------------------------------------------------------

    footer: ToolBar {
        implicitHeight: 30
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: Styles.Primitives.spacing.sp150

            ProgressBar {
                Layout.preferredWidth: 160
                visible: Session.state === Session.Indexing || Session.state === Session.Discovering
                indeterminate: Session.state === Session.Discovering
                value: Session.progress
            }
            Label {
                visible: Session.state === Session.Indexing || Session.state === Session.Discovering
                text: Session.progressText
                color: Theme.textMuted
                elide: Text.ElideMiddle
                Layout.maximumWidth: 420
            }
            Button {
                visible: Session.state === Session.Indexing || Session.state === Session.Discovering
                styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                text: qsTr("Stop")
                onClicked: Session.cancel()
            }
            Label {
                visible: Session.hasModel && Session.state === Session.Ready
                color: Theme.textMuted
                text: {
                    const s = Session.stats
                    return qsTr("%1 files · %2 lines · %3 types · %4 functions · %5 references · indexed in %6 s")
                        .arg(s.files).arg(Number(s.codeLines).toLocaleString(Qt.locale(), "f", 0))
                        .arg(s.types).arg(s.functions).arg(Number(s.references).toLocaleString(Qt.locale(), "f", 0))
                        .arg((s.indexMs / 1000).toFixed(1))
                }
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Item { Layout.fillWidth: true; visible: !(Session.hasModel && Session.state === Session.Ready) }

            Button {
                visible: Session.stale && Session.state === Session.Ready
                styles: [Styles.ButtonStyle.outline, Styles.ButtonStyle.small]
                icon.source: Icons.source("refresh-cw")
                text: qsTr("Sources changed: index again")
                onClicked: Session.reindex()
            }
            Button {
                visible: Session.hasModel && (Session.stats.errors > 0 || Session.stats.warnings > 0)
                styles: [Styles.ButtonStyle.ghost, Styles.ButtonStyle.small]
                icon.source: Icons.source("triangle-alert")
                icon.color: Session.stats.errors > 0 ? Theme.danger : Theme.warning
                text: qsTr("%1 errors, %2 warnings").arg(Session.stats.errors).arg(Session.stats.warnings)
                onClicked: errorsDialog.open()
            }
        }
    }

    // --- Dialogs -----------------------------------------------------------------

    FolderDialog {
        id: folderDialog
        title: qsTr("Open a C or C++ project")
        currentFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)
        onAccepted: Session.open(selectedFolder)
    }
    FileDialog {
        id: databaseDialog
        title: qsTr("Open a compilation database")
        nameFilters: [qsTr("Compilation database (compile_commands.json)"), qsTr("All files (*)")]
        onAccepted: Session.open(selectedFile)
    }
    FileDialog {
        id: reportDialog
        title: qsTr("Save the review report")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "md"
        nameFilters: [qsTr("Markdown (*.md)")]
        selectedFile: "file://" + Session.root + "/review.md"
        onAccepted: {
            if (Session.saveReport(selectedFile))
                toast.show(qsTr("Saved the review report"))
        }
    }
    SettingsDialog { id: settingsDialog }
    ErrorsDialog { id: errorsDialog }
    ShortcutsDialog { id: shortcutsDialog }

    DropArea {
        anchors.fill: parent
        onDropped: drop => {
            if (drop.hasUrls && drop.urls.length > 0)
                Session.open(drop.urls[0])
        }
    }

    // A message that goes away by itself
    Rectangle {
        id: toast
        function show(text) {
            toastLabel.text = text
            opacity = 1
            toastTimer.restart()
        }
        parent: Overlay.overlay
        anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
        y: parent ? parent.height - height - 48 : 0
        width: toastLabel.implicitWidth + 32
        height: toastLabel.implicitHeight + 16
        radius: Theme.radius
        color: Theme.s.text_default
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Label {
            id: toastLabel
            anchors.centerIn: parent
            color: Theme.s.background_elevated
        }
        Timer { id: toastTimer; interval: 2800; onTriggered: toast.opacity = 0 }
    }
}
