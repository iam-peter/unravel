pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Unravel

Item {
    id: panel
    property IncludeImpactModel impact: null
    signal evidenceOpened()

    Component.onCompleted: {
        if (!impact)
            impact = defaultImpact.createObject(panel)
    }

    Component {
        id: defaultImpact
        IncludeImpactModel {}
    }

    readonly property int pageSize: 50
    property int unitOffset: 0
    property int unit: -1

    Connections {
        target: panel.impact
        function onSelectionChanged() {
            panel.unitOffset = 0
            panel.unit = -1
        }
    }

    onImpactChanged: {
        unitOffset = 0
        unit = -1
    }

    component EvidenceList: ListView {
        clip: true
        delegate: ItemDelegate {
            id: step
            required property var modelData
            width: ListView.view.width
            text: modelData.line > 0
                  ? qsTr("%1:%2 → %3").arg(modelData.fromPath).arg(modelData.line).arg(modelData.toPath)
                  : qsTr("%1 (line unavailable) → %2").arg(modelData.fromPath).arg(modelData.toPath)
            ToolTip.visible: hovered
            ToolTip.text: text
            onClicked: {
                panel.impact.openEvidence(modelData.from, modelData.line)
                panel.evidenceOpened()
            }
        }
        ScrollBar.vertical: ScrollBar {}
    }

    Loader {
        anchors.fill: parent
        anchors.margins: 12
        active: panel.impact !== null
        sourceComponent: PanelContent {}
    }

    component PanelContent: ColumnLayout {
        Label {
            Layout.fillWidth: true
            text: panel.impact.coverage
            wrapMode: Text.Wrap
        }

        SplitView {
            visible: panel.impact.available
            Layout.fillWidth: true
            Layout.fillHeight: true
            handle: SplitterHandle {}

            ColumnLayout {
                SplitView.preferredWidth: 320
                SplitView.minimumWidth: 160

                Label {
                    text: qsTr("Project headers · estimated TU impact")
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                Label {
                    visible: ranks.count === 0
                    text: qsTr("No project headers to rank.")
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                }
                ListView {
                    id: ranks
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: panel.impact
                    delegate: ItemDelegate {
                        id: rank
                        required property int fileId
                        required property string filePath
                        required property int estimatedUnits
                        required property bool inCycle
                        width: ListView.view.width
                        highlighted: fileId === panel.impact.selectedFile
                        text: inCycle ? qsTr("%1 · %2 (cycle)").arg(estimatedUnits).arg(filePath)
                                      : qsTr("%1 · %2").arg(estimatedUnits).arg(filePath)
                        ToolTip.visible: hovered
                        ToolTip.text: text
                        onClicked: panel.impact.selectFile(fileId)
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }

            ColumnLayout {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 240

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: panel.impact.selectedFile >= 0
                          ? qsTr("%1 — %2 estimated affected TU(s)").arg(panel.impact.selectedPath)
                                                                   .arg(panel.impact.affectedUnitCount)
                          : qsTr("Select a header or open a project file.")
                }

                TabBar {
                    id: tabs
                    Layout.fillWidth: true
                    visible: panel.impact.selectedFile >= 0
                    TabButton { text: qsTr("Witnesses") }
                    TabButton { text: qsTr("Include-only graph") }
                    TabButton { text: qsTr("Cycle") }
                }
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: panel.impact.selectedFile >= 0
                    currentIndex: tabs.currentIndex

                    ColumnLayout {
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: panel.impact.affectedUnitCount > 0
                                  ? qsTr("Select a translation unit to see one recorded include chain.")
                                  : qsTr("No affected translation units found in the recorded includes.")
                        }
                        RowLayout {
                            visible: panel.impact.affectedUnitCount > panel.pageSize
                            Button {
                                text: qsTr("Previous")
                                enabled: panel.unitOffset > 0
                                onClicked: {
                                    panel.unitOffset -= panel.pageSize
                                    panel.unit = -1
                                }
                            }
                            Label {
                                text: qsTr("%1–%2 of %3").arg(panel.unitOffset + 1)
                                     .arg(Math.min(panel.unitOffset + panel.pageSize, panel.impact.affectedUnitCount))
                                     .arg(panel.impact.affectedUnitCount)
                            }
                            Button {
                                text: qsTr("Next")
                                enabled: panel.unitOffset + panel.pageSize < panel.impact.affectedUnitCount
                                onClicked: {
                                    panel.unitOffset += panel.pageSize
                                    panel.unit = -1
                                }
                            }
                        }
                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: {
                                // Tie the invokable result to focus changes, even when counts match.
                                const file = panel.impact.selectedFile
                                return file < 0 ? [] : panel.impact.units(panel.unitOffset, panel.pageSize)
                            }
                            delegate: ItemDelegate {
                                required property var modelData
                                width: ListView.view.width
                                text: modelData.filePath
                                highlighted: panel.unit === modelData.fileId
                                onClicked: panel.unit = modelData.fileId
                            }
                            ScrollBar.vertical: ScrollBar {}
                        }
                        EvidenceList {
                            Layout.fillWidth: true
                            Layout.fillHeight: panel.unit >= 0
                            visible: panel.unit >= 0
                            model: panel.unit < 0 || panel.impact.selectedFile < 0 ? [] : panel.impact.witness(panel.unit)
                        }
                    }

                    ColumnLayout {
                        RowLayout {
                            ComboBox {
                                model: [qsTr("Includers"), qsTr("Includes")]
                                currentIndex: panel.impact.showIncludes ? 1 : 0
                                onActivated: index => panel.impact.showIncludes = index === 1
                            }
                            Button {
                                text: qsTr("Expand")
                                enabled: panel.impact.graphInfo.depthTruncated || panel.impact.graphInfo.nodesTruncated
                                onClicked: panel.impact.expandGraph()
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: qsTr("%1 files, %2 recorded links (depth limit %3, file limit %4).")
                                  .arg(panel.impact.graphInfo.nodeCount).arg(panel.impact.graphInfo.edgeCount)
                                  .arg(panel.impact.graphInfo.depthLimit).arg(panel.impact.graphInfo.nodeLimit)
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            visible: panel.impact.graphInfo.depthTruncated || panel.impact.graphInfo.nodesTruncated
                            text: panel.impact.graphInfo.depthTruncated && panel.impact.graphInfo.nodesTruncated
                                  ? qsTr("More files omitted by depth and file limits.")
                                  : panel.impact.graphInfo.depthTruncated ? qsTr("More files omitted by the depth limit.")
                                                                         : qsTr("More files omitted by the file limit.")
                        }
                        EvidenceList {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            model: panel.impact.graphEdges
                        }
                    }

                    ColumnLayout {
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: panel.impact.cycleSteps.length
                                  ? qsTr("The selected file is in a recorded include cycle. %1 files are mutually reachable; one cycle is shown.")
                                    .arg(panel.impact.componentSize)
                                  : qsTr("No recorded include cycle through the selected file.")
                        }
                        EvidenceList {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            model: panel.impact.cycleSteps
                        }
                    }
                }
            }
        }
    }
}
