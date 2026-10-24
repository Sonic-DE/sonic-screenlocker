/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtQuick.Window
import QtQuick.VirtualKeyboard.Settings
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents

Window {
    width: 1000
    height: 700
    visible: true
    color: "#803040"
    Kirigami.Theme.inherit: false
    Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
    readonly property color unlockTextColor: unlock.Kirigami.Theme.textColor
    readonly property color fieldBackgroundColor: password.Kirigami.Theme.backgroundColor
    readonly property string inputLocale: VirtualKeyboardSettings.locale
    readonly property url layoutPath: VirtualKeyboardSettings.layoutPath
    PlasmaComponents.Button {
        id: unlock
        objectName: "unlock"
        x: 40
        y: 100
        width: 200
        height: 60
        text: "Unlock"
        focusPolicy: Qt.NoFocus
    }
    PlasmaComponents.TextField {
        id: password
        objectName: "password"
        x: 40
        y: 30
        width: 500
        height: 40
        focus: true
        echoMode: TextInput.Password
        inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
    }
}
