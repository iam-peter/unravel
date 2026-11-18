pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

ColumnLayout {
    id: view
    property bool active: false
    spacing: 0
    TabBar {
        id: tabs
        Layout.fillWidth: true
        TabButton { text: qsTr("Calls"); styles: [Styles.TabStyle.small] }
        TabButton { text: qsTr("Relationships"); styles: [Styles.TabStyle.small] }
        TabButton { text: qsTr("Qt events"); styles: [Styles.TabStyle.small] }
        TabButton { text: qsTr("Control flow (experimental)"); styles: [Styles.TabStyle.small] }
    }
    StackLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        currentIndex: tabs.currentIndex
        FlowView { active: view.active && tabs.currentIndex === 0 }
        RelationsPanel {}
        EventsPanel {}
        ControlFlowPanel { active: view.active && tabs.currentIndex === 3 }
    }
}
