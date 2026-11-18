pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

ColumnLayout {
    id: view
    property var connections: []
    Component.onCompleted: connections = Session.eventConnections()
    Connections {
        target: Session
        function onModelChanged() { view.connections = Session.eventConnections() }
    }
    Label {
        Layout.fillWidth: true
        padding: 8
        wrapMode: Text.Wrap
        color: Theme.textMuted
        text: qsTr("Potential Qt event delivery, not immediate execution. Typed QObject::connect sites only; string-based, QML, dynamic invocation and runtime disconnection/receiver lifetime are incomplete. AutoConnection depends on runtime thread affinity.")
    }
    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: view.connections
        ScrollBar.vertical: ScrollBar {}
        delegate: ItemDelegate {
            id: row
            required property var modelData
            width: ListView.view.width
            styles: [Styles.ItemStyle.listRow]
            implicitHeight: content.implicitHeight + 16
            onClicked: Session.activateFile(modelData.file, modelData.line)
            contentItem: ColumnLayout {
                id: content
                Label {
                    Layout.fillWidth: true
                    text: row.modelData.signalName + " -> " + row.modelData.targetName
                    font.weight: Styles.Primitives.fontWeight.semiBold
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    text: row.modelData.sender + " -> " + row.modelData.receiver + " | " + row.modelData.delivery
                          + (row.modelData.limitation ? "\n" + row.modelData.limitation : "")
                    color: Theme.textMuted
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Button {
                        text: qsTr("Signal")
                        enabled: row.modelData.signal >= 0
                        onClicked: Session.activate(row.modelData.signal)
                    }
                    Button {
                        text: qsTr("Target")
                        onClicked: {
                            if (row.modelData.target >= 0) Session.activate(row.modelData.target)
                            else Session.activateFile(row.modelData.targetFile, row.modelData.targetLine)
                        }
                    }
                }
            }
        }
        Label {
            anchors.centerIn: parent
            visible: view.connections.length === 0
            text: qsTr("No supported connection sites in the current index")
            color: Theme.textMuted
        }
    }
}
