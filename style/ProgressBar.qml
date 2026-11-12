import QtQuick
import QtQuick.Templates as T

T.ProgressBar {
    id: control

    property list<Style> styles

    StyleCascade {
        id: _style
        base: [ProgressBarStyle.base]
        styles: control.styles
    }
    StyleCascade {
        id: _trackStyle
        parentCascade: _style
        part: StylePart.Track
    }
    StyleCascade {
        id: _progressStyle
        parentCascade: _style
        part: StylePart.Progress
    }

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    contentItem: Item {
        id: track
        implicitWidth: _style.width
        implicitHeight: _style.height

        // Where the indeterminate sweep's left end is; it may run outside
        // the track, the bar is cut to it rather than clipped, so both its
        // ends stay round. A determinate bar always starts at 0: the sweep
        // must not leave it shifted when the bar stops being indeterminate.
        property real sweep: 0
        readonly property real fillLeft: control.indeterminate ? Math.max(0, sweep) : 0
        readonly property real fillRight: control.indeterminate ? Math.min(width, sweep + width / 4)
                                                            : control.visualPosition * width

        NumberAnimation on sweep {
            running: control.indeterminate && control.visible
            loops: Animation.Infinite
            from: -track.width / 4
            to: track.width
            duration: 1400
        }

        Rectangle {
            x: track.fillLeft
            width: Math.max(0, track.fillRight - track.fillLeft)
            height: parent.height
            radius: Math.min(_style.radius.value, width / 2)
            color: _progressStyle.background
        }
    }

    background: Rectangle {
        implicitWidth: _style.width
        implicitHeight: _style.height
        y: (control.height - height) / 2
        height: _style.height
        radius: _style.radius.value
        color: _trackStyle.background
    }
}
