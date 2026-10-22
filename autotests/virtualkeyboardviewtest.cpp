/*
 * SPDX-FileCopyrightText: 2026 SonicDE Community
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../greeter/virtualkeyboard.h"

#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTest>

class VirtualKeyboardViewTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*(TypeError|ReferenceError).*")));
    }

    void invalidAndDuplicateViews()
    {
        QQmlEngine engine;
        QQuickWindow view;
        ScreenLocker::VirtualKeyboard keyboard;
        view.resize(1920, 1080);
        view.show();
        QTRY_COMPARE(view.contentItem()->width(), 1920);
        keyboard.addView(nullptr, &engine);
        keyboard.addView(&view, nullptr);
        QVERIFY(!keyboard.panel());
        keyboard.addView(&view, &engine);
        QVERIFY(keyboard.panel());
        const auto panel = keyboard.panel();
        keyboard.addView(&view, &engine);
        QCOMPARE(keyboard.panel(), panel);
        QCOMPARE(panel->window(), &view);
        QTRY_COMPARE(panel->width(), 1920);
        QTRY_COMPARE(panel->y() + panel->height(), 1080);
        view.resize(1280, 720);
        QTRY_COMPARE(panel->width(), 1280);
        QTRY_COMPARE(panel->y() + panel->height(), 720);
        QVERIFY(panel->height() > 0);
    }

    void onlyKnownViewsReceivePanel()
    {
        QQmlEngine engine;
        QQuickWindow first;
        QQuickWindow second;
        QQuickWindow unregistered;
        ScreenLocker::VirtualKeyboard keyboard;
        first.resize(1920, 1080);
        second.resize(1280, 720);
        first.show();
        second.show();
        QTRY_COMPARE(second.contentItem()->width(), 1280);
        keyboard.addView(&first, &engine);
        keyboard.addView(&second, &engine);
        const auto panel = keyboard.panel();
        QVERIFY(panel);
        // Exercise the focus notification deterministically, without asking a
        // window manager to focus a window or depending on Xvfb timing.
        Q_EMIT qGuiApp->focusWindowChanged(&second);
        QCOMPARE(keyboard.panel(), panel);
        QCOMPARE(panel->window(), &second);
        QTRY_COMPARE(panel->width(), 1280);
        QTRY_COMPARE(panel->y() + panel->height(), 720);
        Q_EMIT qGuiApp->focusWindowChanged(&unregistered);
        QCOMPARE(panel->window(), &second);
        Q_EMIT qGuiApp->focusWindowChanged(nullptr);
        QCOMPARE(panel->window(), &second);
        Q_EMIT qGuiApp->focusWindowChanged(&first);
        QCOMPARE(keyboard.panel(), panel);
        QCOMPARE(panel->window(), &first);
        QTRY_COMPARE(panel->width(), 1920);
        keyboard.removeView(&first);
        QVERIFY(keyboard.panel());
        QCOMPARE(keyboard.panel()->window(), &second);
        keyboard.removeView(&second);
        QVERIFY(!keyboard.panel());
        Q_EMIT qGuiApp->focusWindowChanged(&unregistered);
        QVERIFY(!keyboard.panel());
    }

    void destroyedViewAndEngine()
    {
        QQuickWindow surviving;
        QQmlEngine survivingEngine;
        ScreenLocker::VirtualKeyboard keyboard;
        {
            QQmlEngine engine;
            QQuickWindow temporary;
            keyboard.addView(&temporary, &engine);
            keyboard.addView(&surviving, &survivingEngine);
            QVERIFY(keyboard.panel());
            Q_EMIT qGuiApp->focusWindowChanged(&surviving);
            QCOMPARE(keyboard.panel()->window(), &surviving);
            keyboard.removeView(&temporary);
            QVERIFY(keyboard.panel());
            QCOMPARE(keyboard.panel()->window(), &surviving);
        }
        Q_EMIT qGuiApp->focusWindowChanged(&surviving);
        QVERIFY(keyboard.panel());
        QCOMPARE(keyboard.panel()->window(), &surviving);
        keyboard.removeView(&surviving);
        QVERIFY(!keyboard.panel());
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_IM_MODULE", "sonicvirtualkeyboard");
    qputenv("QT_VIRTUALKEYBOARD_DESKTOP_DISABLE", "1");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QGuiApplication app(argc, argv);
    VirtualKeyboardViewTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "virtualkeyboardviewtest.moc"
