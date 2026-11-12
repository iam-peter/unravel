pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// What shows without a project: open one, or one of the recent ones
Rectangle {
    id: page

    signal openFolder()
    signal openDatabase()

    color: Theme.canvas

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(640, parent.width - 64)
        spacing: Styles.Primitives.spacing.sp200

        RowLayout {
            spacing: Styles.Primitives.spacing.sp200
            Image {
                source: "qrc:/qt/qml/Unravel/logo/unravel.svg"
                sourceSize: Qt.size(72, 72)
            }
            ColumnLayout {
                spacing: 2
                Label {
                    text: "Unravel"
                    font.pixelSize: 30
                    font.weight: Styles.Primitives.fontWeight.bold
                }
                Label {
                    text: qsTr("See the architecture, flows and connections of C, C++ and Python code")
                    color: Theme.textMuted
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textMuted
            text: qsTr("Open a project folder: unravel looks for a compile_commands.json in it and in its build folders, asks CMake for one when there is a CMakeLists.txt, or else compiles every source with include paths it finds. clang does the parsing, on all cores; Python files are read with Python's own ast, beside them. The index is kept for next time.")
        }

        RowLayout {
            spacing: Styles.Primitives.spacing.sp100
            Button {
                styles: [Styles.ButtonStyle.primary]
                icon.source: Icons.source("folder-open")
                text: qsTr("Open project folder…")
                onClicked: page.openFolder()
            }
            Button {
                styles: [Styles.ButtonStyle.outline]
                icon.source: Icons.source("file-code")
                text: qsTr("Open compile_commands.json…")
                onClicked: page.openDatabase()
            }
        }

        Label {
            visible: Session.state === Session.Indexing || Session.state === Session.Discovering
            text: Session.progressText
            color: Theme.textMuted
        }
        ProgressBar {
            Layout.fillWidth: true
            visible: Session.state === Session.Indexing || Session.state === Session.Discovering
            indeterminate: Session.state === Session.Discovering
            value: Session.progress
        }
        Label {
            Layout.fillWidth: true
            visible: Session.error !== ""
            text: Session.error
            color: Theme.danger
            wrapMode: Text.Wrap
        }
        Repeater {
            model: Session.notes
            Label {
                required property string modelData
                Layout.fillWidth: true
                text: modelData
                color: Theme.textMuted
                font: Theme.smallFont
                wrapMode: Text.Wrap
            }
        }

        Label {
            visible: Session.recentProjects.length > 0
            text: qsTr("Recent")
            font.weight: Styles.Primitives.fontWeight.semiBold
            Layout.topMargin: Styles.Primitives.spacing.sp100
        }
        Frame {
            Layout.fillWidth: true
            visible: Session.recentProjects.length > 0
            padding: 2
            ColumnLayout {
                width: parent.width
                spacing: 0
                Repeater {
                    model: Session.recentProjects
                    ItemDelegate {
                        id: recent
                        required property string modelData
                        Layout.fillWidth: true
                        styles: [Styles.ItemStyle.listRow]
                        icon.source: Icons.source("folder")
                        text: modelData
                        onClicked: Session.open(modelData)
                        IconButton {
                            anchors.right: parent.right
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            iconName: "x"
                            visible: recent.hovered
                            onClicked: Session.forgetRecent(recent.modelData)
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Remove from the list")
                        }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            color: Theme.textMuted
            font: Theme.smallFont
            wrapMode: Text.Wrap
            text: qsTr("Or drop a folder or a compile_commands.json on the window. From a terminal: unravel path/to/project")
        }
    }
}
