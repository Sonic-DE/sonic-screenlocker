/*
 * SPDX-FileCopyrightText: 2026 SonicDE Community
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
import QtQuick
import QtQuick.VirtualKeyboard
import QtQuick.VirtualKeyboard.Settings

// Application integration: this is part of the already registered greeter
// surface, not an auxiliary X11 window which the lock must keep underneath it.
InputPanel {
    objectName: "screenlockerVirtualKeyboard"
    readonly property string keyboardStyleName: VirtualKeyboardSettings.styleName
    anchors.left: parent ? parent.left : undefined
    anchors.right: parent ? parent.right : undefined
    anchors.bottom: parent ? parent.bottom : undefined
    z: 10000
    visible: Qt.inputMethod.visible
}
