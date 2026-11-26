// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

KCM.SimpleKCM {
    id: page

    readonly property bool recording: kcm.phase === "capture"
    readonly property bool ready: kcm.phase === "ready"
    readonly property bool verifying: kcm.phase === "verifying"
    readonly property bool saved: kcm.phase === "saved"
    readonly property bool failed: kcm.phase === "error"
    readonly property int collected: kcm.captured
    readonly property color accent: Kirigami.Theme.highlightColor
    readonly property color positive: Kirigami.Theme.positiveTextColor
    readonly property bool animate: Kirigami.Units.longDuration > 0
    readonly property bool wide: content.width >= 690

    function titleFor(phase) {
        switch (phase) {
        case "permission": return i18nd("kcm_surfaceir", "Waiting for permission")
        case "capture": return i18nd("kcm_surfaceir", "Collect face samples")
        case "ready": return i18nd("kcm_surfaceir", "Ready for verification")
        case "verifying": return i18nd("kcm_surfaceir", "Checking your face")
        case "saved": return i18nd("kcm_surfaceir", "Face samples saved")
        case "cancelling": return i18nd("kcm_surfaceir", "Cancelling")
        case "cancelled": return i18nd("kcm_surfaceir", "Enrollment cancelled")
        case "error": return i18nd("kcm_surfaceir", "Enrollment could not finish")
        default: return i18nd("kcm_surfaceir", "Add your face")
        }
    }

    function guideFor(phase, hint) {
        switch (phase) {
        case "permission": return i18nd("kcm_surfaceir", "Approve camera access in the system dialog.")
        case "capture":
            switch (hint) {
            case "warmup": return i18nd("kcm_surfaceir", "Adjusting the infrared exposure…")
            case "hold": return i18nd("kcm_surfaceir", "Hold still and keep your face in the guide.")
            case "vary": return i18nd("kcm_surfaceir", "Turn your head slightly for another angle.")
            case "captured": return i18nd("kcm_surfaceir", "Sample captured. Try a slightly different angle.")
            default: return i18nd("kcm_surfaceir", "Center your face and look at the camera above the screen.")
            }
        case "ready": return i18nd("kcm_surfaceir", "Return to your usual unlock position, then verify the new samples.")
        case "verifying": return i18nd("kcm_surfaceir", "Keep looking at the camera while we check a fresh frame.")
        case "saved": return i18nd("kcm_surfaceir", "%1 face samples are stored on this device.", kcm.total)
        case "cancelling": return i18nd("kcm_surfaceir", "Stopping the camera…")
        case "cancelled": return i18nd("kcm_surfaceir", "Your existing face samples are still available.")
        case "error":
            switch (hint) {
            case "timeout": return i18nd("kcm_surfaceir", "Five samples could not be captured. Try again from a steady position.")
            case "nomatch": return i18nd("kcm_surfaceir", "The live check did not match. Try again from your usual unlock position.")
            case "authorization": return i18nd("kcm_surfaceir", "Administrator permission is required to open the camera.")
            case "unavailable": return i18nd("kcm_surfaceir", "The enrollment helper could not be started.")
            default: return i18nd("kcm_surfaceir", "Check that the infrared camera is available and try again.")
            }
        default: return i18nd("kcm_surfaceir", "Sit at your usual unlock distance. Turn your head slightly between captures.")
        }
    }

    ColumnLayout {
        id: content
        spacing: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing

        ColumnLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.topMargin: 4
            Layout.bottomMargin: 3

            Kirigami.Heading {
                level: 1
                font.pixelSize: Math.max(23, Kirigami.Theme.defaultFont.pixelSize + 8)
                font.weight: Font.DemiBold
                text: i18nd("kcm_surfaceir", "Infrared face enrollment")
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }

            Controls.Label {
                text: i18nd("kcm_surfaceir", "Set up face unlock for the KDE lock screen.")
                opacity: 0.77
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: page.wide ? 2 : 1
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.largeSpacing

            Rectangle {
                id: cameraCard
                objectName: "surfaceIrCameraCard"
                Layout.fillWidth: true
                Layout.preferredWidth: 410
                Layout.minimumWidth: 0
                Layout.preferredHeight: Math.max(228, Math.min(380, width * 0.72))
                radius: 14
                color: "#131b27"
                border.color: "#344359"
                clip: true

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: cameraCard.radius - 1
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#1a2838" }
                        GradientStop { position: 1; color: "#101722" }
                    }
                }

                Image {
                    id: cameraImage
                    anchors.fill: parent
                    anchors.margins: 1
                    fillMode: Image.PreserveAspectFit
                    source: kcm.preview
                    cache: false
                    smooth: true
                    visible: status === Image.Ready
                    Accessible.ignored: true
                }

                Item {
                    id: viewfinder
                    width: Math.min(parent.width * 0.53, parent.height * 0.58)
                    height: width * 1.10
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -8
                    opacity: page.saved || page.failed || page.verifying ? 0 : 1
                    visible: opacity > 0

                    Behavior on opacity { NumberAnimation { duration: page.animate ? 170 : 0 } }

                    Rectangle {
                        anchors.fill: parent
                        radius: 25
                        color: "transparent"
                        border.width: 1
                        border.color: "#6382a3"
                        opacity: 0.5
                    }

                    Repeater {
                        model: 4
                        delegate: Item {
                            required property int index
                            x: index % 2 === 0 ? 0 : viewfinder.width - width
                            y: index < 2 ? 0 : viewfinder.height - height
                            width: 21
                            height: width

                            Rectangle {
                                x: 0
                                y: index < 2 ? 0 : parent.height - height
                                width: parent.width
                                height: 2
                                radius: 1
                                color: page.recording ? "#91e9dd" : "#92b4d0"
                            }
                            Rectangle {
                                x: index % 2 === 0 ? 0 : parent.width - width
                                width: 2
                                height: parent.height
                                radius: 1
                                color: page.recording ? "#91e9dd" : "#92b4d0"
                            }
                        }
                    }

                    Item {
                        anchors.centerIn: parent
                        width: 54
                        height: 68
                        visible: !cameraImage.visible

                        Rectangle {
                            width: 37
                            height: 46
                            radius: 17
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 2
                            color: "transparent"
                            border.color: "#a6c6de"
                            border.width: 2

                            Repeater {
                                model: 2
                                delegate: Rectangle {
                                    required property int index
                                    x: index === 0 ? 10 : 23
                                    y: 18
                                    width: 3
                                    height: width
                                    radius: width / 2
                                    color: "#a6c6de"
                                }
                            }
                        }

                        Rectangle {
                            width: 18
                            height: 2
                            radius: 1
                            color: "#789bb8"
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                    }

                    Rectangle {
                        id: scan
                        property real position: 0
                        x: 9
                        width: parent.width - 18
                        height: 2
                        y: 10 + position * (parent.height - 20)
                        radius: 1
                        color: "#9af6e4"
                        opacity: 0.85
                        visible: page.recording && cameraImage.visible && page.animate

                        SequentialAnimation on position {
                            running: scan.visible
                            loops: Animation.Infinite
                            NumberAnimation { from: 0; to: 1; duration: 1450; easing.type: Easing.InOutSine }
                            NumberAnimation { from: 1; to: 0; duration: 1450; easing.type: Easing.InOutSine }
                        }
                    }
                }

                Rectangle {
                    id: cameraFlag
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.margins: 14
                    width: flagText.implicitWidth + 22
                    height: flagText.implicitHeight + 10
                    radius: height / 2
                    color: "#df172332"
                    border.color: "#47617b"

                    Controls.Label {
                        id: flagText
                        anchors.centerIn: parent
                        color: "#eff7ff"
                        font.pixelSize: Math.max(11, Kirigami.Theme.smallFont.pixelSize)
                        text: page.recording && cameraImage.visible ? i18nd("kcm_surfaceir", "Live infrared") : i18nd("kcm_surfaceir", "Infrared camera")
                    }
                }

                ColumnLayout {
                    width: parent.width - 36
                    anchors.centerIn: parent
                    spacing: 8
                    visible: page.saved || page.failed || page.verifying

                    Kirigami.Icon {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 57
                        Layout.preferredHeight: 57
                        color: page.saved ? "#92ecdb" : "#e8eef6"
                        source: page.saved ? "dialog-ok-apply" : "edit-image-face-detect-symbolic"
                        visible: !page.verifying
                    }

                    Controls.BusyIndicator {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 51
                        Layout.preferredHeight: 51
                        running: page.verifying
                        visible: running
                    }
                }

                Rectangle {
                    id: flash
                    anchors.fill: parent
                    opacity: 0
                    color: "#c9fcf3"
                    radius: cameraCard.radius

                    NumberAnimation { id: feedback; target: flash; property: "opacity"; from: 0.24; to: 0; duration: 380 }
                }

                Connections {
                    target: page
                    function onCollectedChanged() {
                        if (page.recording && page.collected > 0 && page.animate)
                            feedback.restart()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredWidth: 270
                Layout.minimumWidth: page.wide ? 230 : 0
                Layout.fillHeight: page.wide
                implicitHeight: details.implicitHeight + 32
                color: Kirigami.Theme.alternateBackgroundColor
                radius: 14
                border.color: Qt.tint(color, Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g,
                                                    Kirigami.Theme.textColor.b, 0.13))

                ColumnLayout {
                    id: details
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 13

                    Kirigami.Heading {
                        level: 2
                        text: page.titleFor(kcm.phase)
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                    }

                    Controls.Label {
                        text: page.guideFor(kcm.phase, kcm.hint)
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.83
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 7

                        RowLayout {
                            Layout.fillWidth: true

                            Controls.Label {
                                Layout.fillWidth: true
                                text: i18nd("kcm_surfaceir", "Face samples")
                                wrapMode: Text.Wrap
                                font.bold: true
                            }

                            Controls.Label {
                                text: i18nd("kcm_surfaceir", "%1 of %2", page.collected, 5)
                                color: page.collected === 5 ? page.positive : Kirigami.Theme.textColor
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 5

                            Repeater {
                                model: 5
                                delegate: Rectangle {
                                    required property int index
                                    readonly property bool complete: page.collected > index
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 5
                                    radius: height / 2
                                    color: complete ? page.accent : Qt.tint(details.parent.color,
                                        Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g,
                                                Kirigami.Theme.textColor.b, 0.18))
                                    Behavior on color { ColorAnimation { duration: page.animate ? 230 : 0 } }
                                }
                            }
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        Layout.preferredHeight: implicitHeight
                        spacing: 7

                        Controls.Button {
                            icon.name: page.ready ? "dialog-ok-apply" : "list-add"
                            highlighted: true
                            implicitHeight: 38
                            implicitWidth: Math.max(130, contentItem.implicitWidth + 26)
                            contentItem: Controls.Label {
                                text: parent.text
                                color: Kirigami.Theme.highlightedTextColor
                                font.weight: Font.DemiBold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 8
                                color: parent.down ? Qt.darker(page.accent, 1.16) : page.accent
                                border.width: parent.visualFocus ? 2 : 0
                                border.color: Kirigami.Theme.textColor
                            }
                            text: page.ready ? i18nd("kcm_surfaceir", "Verify and save")
                                             : page.saved ? i18nd("kcm_surfaceir", "Add more samples") : i18nd("kcm_surfaceir", "Start enrollment")
                            visible: !kcm.busy || page.ready
                            onClicked: page.ready ? kcm.verify() : kcm.enroll()
                        }

                        Controls.Button {
                            text: i18nd("kcm_surfaceir", "Cancel")
                            implicitHeight: 38
                            visible: kcm.busy
                            enabled: kcm.phase !== "cancelling"
                            onClicked: kcm.cancel()
                        }
                    }

                    Item { Layout.fillHeight: page.wide }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: Qt.tint(details.parent.color, Qt.rgba(Kirigami.Theme.textColor.r,
                            Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.13))
                    }

                    Controls.Label {
                        Layout.fillWidth: true
                        text: i18nd("kcm_surfaceir", "Preview frames stay in memory. Face samples are saved on this device after verification.")
                        wrapMode: Text.Wrap
                        opacity: 0.73
                    }
                }
            }
        }

    }
}
