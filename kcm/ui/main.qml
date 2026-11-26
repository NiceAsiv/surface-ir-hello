// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

KCM.SimpleKCM {
    id: page

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: i18nd("kcm_surfaceir", "Look at the infrared camera above the screen. Change your head angle slightly after each captured sample.")
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 13
            color: Kirigami.Theme.alternateBackgroundColor
            radius: Kirigami.Units.smallSpacing

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Kirigami.Units.iconSizes.huge
                height: width
                source: "edit-image-face-detect-symbolic"
                visible: preview.status !== Image.Ready
            }

            Image {
                id: preview
                anchors.fill: parent
                source: kcm.preview
                fillMode: Image.PreserveAspectFit
                cache: false
                visible: status === Image.Ready
            }
        }

        Controls.ProgressBar {
            Layout.fillWidth: true
            visible: kcm.busy
            from: 0
            to: 5
            value: kcm.captured
        }

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: kcm.status.length > 0
            text: kcm.status
        }

        RowLayout {
            Controls.Button {
                text: i18nd("kcm_surfaceir", "Enroll face")
                icon.name: "list-add"
                enabled: !kcm.busy
                onClicked: kcm.enroll()
            }
            Controls.Button {
                text: i18nd("kcm_surfaceir", "Verify and save")
                icon.name: "dialog-ok-apply"
                enabled: kcm.canVerify
                onClicked: kcm.verify()
            }
            Controls.Button {
                text: i18nd("kcm_surfaceir", "Cancel")
                enabled: kcm.busy
                onClicked: kcm.cancel()
            }
        }
    }
}
