pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import IgittStyle as Styles
import Unravel

Item {
    id: panel

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            Layout.fillWidth: true
            padding: Styles.Primitives.spacing.sp150
            background: Rectangle { color: Theme.s.background_default }
            ColumnLayout {
                width: parent.width
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Architecture contracts"); font.weight: Styles.Primitives.fontWeight.semiBold }
                        Label {
                            Layout.fillWidth: true
                            text: Contracts.configured ? Contracts.configPath
                                                       : qsTr("No .unravel/architecture.json — observed architecture is unchanged")
                            color: Theme.textMuted
                            font: Theme.smallFont
                            elide: Text.ElideMiddle
                        }
                    }
                    Button {
                        styles: [Styles.ButtonStyle.outline, Styles.ButtonStyle.small]
                        text: qsTr("Reload")
                        enabled: !Contracts.busy
                        onClicked: Contracts.reload()
                    }
                    Button {
                        styles: [Styles.ButtonStyle.outline, Styles.ButtonStyle.small]
                        text: qsTr("Copy JSON")
                        enabled: !Contracts.busy
                        onClicked: Session.copyToClipboard(Contracts.reportJson())
                    }
                    Button {
                        styles: [Styles.ButtonStyle.outline, Styles.ButtonStyle.small]
                        text: qsTr("Copy Markdown")
                        enabled: !Contracts.busy
                        onClicked: Session.copyToClipboard(Contracts.reportMarkdown())
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: Contracts.busy || !Contracts.valid || Contracts.violations.length === 0
                    wrapMode: Text.Wrap
                    color: Contracts.valid ? Theme.textMuted : Theme.danger
                    text: Contracts.busy ? qsTr("Checking architecture contracts…")
                        : !Contracts.valid ? Contracts.error
                        : Contracts.configured ? qsTr("No forbidden dependencies.")
                        : qsTr("No contract rules configured.")
                }
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical
            handle: SplitterHandle {}

            ListView {
                id: violations
                SplitView.fillHeight: true
                clip: true
                model: Contracts.violations
                ScrollBar.vertical: ScrollBar {}
                header: Label {
                    width: ListView.view.width
                    leftPadding: 12
                    topPadding: 12
                    text: qsTr("Forbidden dependencies")
                    font.weight: Styles.Primitives.fontWeight.semiBold
                }
                delegate: ColumnLayout {
                    id: violationDelegate
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    spacing: 2
                    Label {
                        Layout.fillWidth: true
                        leftPadding: 12
                        topPadding: 8
                        text: "%1  %2 → %3  (%4)"
                                  .arg(violationDelegate.modelData.ruleId)
                                  .arg(violationDelegate.modelData.sourceLayer)
                                  .arg(violationDelegate.modelData.targetLayer)
                                  .arg(violationDelegate.modelData.scope)
                        color: Theme.danger
                        font.weight: Styles.Primitives.fontWeight.semiBold
                    }
                    Repeater {
                        model: violationDelegate.modelData.evidence
                        ItemDelegate {
                            required property var modelData
                            required property int index
                            width: violationDelegate.width
                            styles: [Styles.ItemStyle.listRow]
                            text: "%1:%2 → %3"
                                      .arg(modelData.sourceFile)
                                      .arg(modelData.line)
                                      .arg(modelData.targetFile)
                            onClicked: Contracts.openEvidence(violationDelegate.index, index)
                        }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: violations.count === 0 && Contracts.valid && !Contracts.busy
                    text: Contracts.configured ? qsTr("No forbidden dependencies") : qsTr("No rules configured")
                    color: Theme.textMuted
                }
            }

            ListView {
                id: unassigned
                SplitView.preferredHeight: 160
                clip: true
                model: Contracts.unassignedFiles
                ScrollBar.vertical: ScrollBar {}
                header: Label {
                    width: ListView.view.width
                    leftPadding: 12
                    topPadding: 10
                    text: qsTr("Unassigned project files (%1)").arg(unassigned.count)
                    font.weight: Styles.Primitives.fontWeight.semiBold
                }
                delegate: ItemDelegate {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    styles: [Styles.ItemStyle.listRow]
                    text: modelData.path
                    onClicked: Contracts.openUnassigned(index)
                }
            }
        }
    }
}
