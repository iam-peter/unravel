pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

// Theme, fonts, the editor and how the project is compiled. Every setting
// takes effect at once.
Dialog {
    id: dialog

    title: qsTr("Settings")
    modal: true
    closeButton: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 580

    ColumnLayout {
        width: parent.width
        spacing: Styles.Primitives.spacing.sp100

        Label { text: qsTr("Appearance"); font.bold: true }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Styles.Primitives.spacing.sp150
            rowSpacing: Styles.Primitives.spacing.sp100

            Label { text: qsTr("Theme") }
            ComboBox {
                Layout.fillWidth: true
                textRole: "name"
                valueRole: "id"
                model: [{id: "system", name: qsTr("System")}].concat(Styles.Themes.available)
                currentIndex: count > 0 ? Math.max(0, indexOfValue(Styles.Themes.theme)) : -1
                onActivated: Styles.Themes.theme = currentValue
            }
            Item { width: 1; height: 1 }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.textMuted
                font.pixelSize: Styles.Primitives.fontSize.small
                text: qsTr("igitt's themes, as Cache uses them. Your own go in %1 as JSON files.").arg(Styles.Themes.folder)
            }
            Label { text: qsTr("Interface font") }
            RowLayout {
                TextField { Layout.fillWidth: true; text: Theme.prefs.uiFamily; onEditingFinished: Theme.prefs.uiFamily = text }
                SpinBox { from: 7; to: 20; value: Theme.prefs.uiSize; onValueModified: Theme.prefs.uiSize = value }
            }
            Label { text: qsTr("Code font") }
            RowLayout {
                TextField { Layout.fillWidth: true; text: Theme.prefs.codeFamily; onEditingFinished: Theme.prefs.codeFamily = text }
                SpinBox { from: 7; to: 20; value: Theme.prefs.codeSize; onValueModified: Theme.prefs.codeSize = value }
            }
        }
        Switch {
            text: qsTr("The mouse wheel zooms the graphs (otherwise it scrolls; Ctrl+wheel always zooms)")
            checked: Theme.prefs.wheelZooms
            onToggled: Theme.prefs.wheelZooms = checked
        }
        Switch {
            text: qsTr("Dot grid behind the graphs")
            checked: Theme.prefs.showGrid
            onToggled: Theme.prefs.showGrid = checked
        }

        MenuSeparator { Layout.fillWidth: true }
        Label { text: qsTr("Editor"); font.bold: true }
        TextField {
            Layout.fillWidth: true
            text: Session.editorCommand
            placeholderText: "code -g {file}:{line}"
            onEditingFinished: Session.editorCommand = text
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textMuted
            font.pixelSize: Styles.Primitives.fontSize.small
            text: qsTr("The command that opens a file at a line: {file} and {line} are filled in. For example “code -g {file}:{line}”, “qtcreator {file}:{line}”, “kate -l {line} {file}”, “subl {file}:{line}”. Empty: the system's default application.")
        }

        MenuSeparator { Layout.fillWidth: true }
        Label { text: qsTr("Compiling this project"); font.bold: true }
        TextField {
            Layout.fillWidth: true
            enabled: Session.hasModel || Session.projectPath !== ""
            text: Session.extraArgs
            placeholderText: "-DFOO=1 -I/opt/include -std=c++23"
            onEditingFinished: Session.extraArgs = text
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textMuted
            font.pixelSize: Styles.Primitives.fontSize.small
            text: qsTr("Arguments clang gets for every file of this project, after those of the compilation database. They apply when it is indexed again (F5).")
        }
        Repeater {
            model: Session.notes
            Label {
                required property string modelData
                Layout.fillWidth: true
                text: "· " + modelData
                color: Theme.textMuted
                font.pixelSize: Styles.Primitives.fontSize.small
                wrapMode: Text.Wrap
            }
        }
        Label {
            Layout.fillWidth: true
            visible: Session.hasModel
            text: qsTr("The index is kept in %1").arg(Session.cachePath())
            color: Theme.textMuted
            font.pixelSize: Styles.Primitives.fontSize.small
            wrapMode: Text.WrapAnywhere
        }
    }
}
