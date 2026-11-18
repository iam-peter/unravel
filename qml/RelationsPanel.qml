pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

ColumnLayout {
    id: view
    property var result: ({})
    function refresh() {
        result = Session.queryRelations(Session.activeSymbol, kinds[query.currentIndex], depth.value, crossModule.checked)
    }
    readonly property var kinds: ["callers", "callees", "subclasses", "overrides", "readers", "writers", "dependencies"]
    Component.onCompleted: refresh()
    Connections {
        target: Session
        function onActiveChanged() { view.refresh() }
        function onModelChanged() { view.refresh() }
    }
    Pane {
        Layout.fillWidth: true
        padding: 8
        background: Rectangle { color: Theme.s.background_default }
        Flow {
            width: parent.width
            spacing: 8
            ComboBox {
                id: query
                model: [qsTr("Callers"), qsTr("Callees"), qsTr("Subclasses / implementors"),
                        qsTr("Overriding methods"), qsTr("Readers"), qsTr("Writers"), qsTr("Dependencies")]
                styles: [Styles.FieldStyle.small]
                onActivated: view.refresh()
            }
            SpinBox { id: depth; from: 1; to: 12; value: 1; onValueModified: view.refresh(); implicitWidth: 100 }
            CheckBox { id: crossModule; text: qsTr("Cross-module results only"); onToggled: view.refresh() }
        }
    }
    Label {
        Layout.fillWidth: true
        padding: 8
        wrapMode: Text.Wrap
        color: Theme.textMuted
        text: (Session.activeInfo.qualified || qsTr("Select a symbol"))
              + (view.result.error ? "\n" + view.result.error : "")
              + (view.result.truncated ? qsTr("\nTruncated at 1,000 symbols; reduce depth.") : "")
              + qsTr("\nIndexed static relationships only; unresolved dynamic targets are not inferred.")
    }
    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        model: view.result.rows || []
        clip: true
        ScrollBar.vertical: ScrollBar {}
        delegate: ItemDelegate {
            required property var modelData
            width: ListView.view.width
            text: modelData.name + " | " + modelData.location + " | " + modelData.distance
            styles: [Styles.ItemStyle.listRow]
            onClicked: Session.activate(modelData.symbol)
        }
    }
}
