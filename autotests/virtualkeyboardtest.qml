/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtQuick.Window
import org.kde.plasma.workspace.loginlockscreen as LoginLockScreen

Window {
    visible: true
    x: Screen.virtualX
    y: Screen.virtualY
    width: 1920
    height: 1080
    flags: Qt.Window | Qt.X11BypassWindowManagerHint
    color: "#803040"
    property var keyboardController: footer.virtualKeyboard
    property int footerActivations: 0
    TextInput {
        id: password
        objectName: "password"
        x: 40; y: 40; width: 500; height: 50
        color: "white"
        focus: true
        echoMode: TextInput.Password
        inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
    }
    LoginLockScreen.Footer {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        onOskActivated: {
            ++footerActivations;
            password.forceActiveFocus();
        }
    }
}
