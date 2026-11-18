pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

ColumnLayout {
    id: view
    property bool active: false
    signal showView(string id)
    spacing: 0
    TabBar {
        id: tabs
        Layout.fillWidth: true
        TabButton { text: qsTr("Findings"); styles: [Styles.TabStyle.small] }
        TabButton { text: qsTr("Review Focus"); styles: [Styles.TabStyle.small] }
        TabButton { text: qsTr("Architecture rules"); styles: [Styles.TabStyle.small] }
        TabButton { text: qsTr("Header impact"); styles: [Styles.TabStyle.small] }
    }
    StackLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        currentIndex: tabs.currentIndex
        FindingsView { active: view.active && tabs.currentIndex === 0; onShowView: id => view.showView(id) }
        FocusView {}
        ContractsPanel {}
        IncludeImpactPanel {}
    }
}
