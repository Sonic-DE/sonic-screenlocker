/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../greeter/greeterappearance.h"
#include "../greeter/virtualkeyboard.h"

#include <KLocalizedQmlContext>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputMethod>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class GreeterAppearanceTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void selectedApplicationPalette()
    {
        QCOMPARE(qGuiApp->palette().color(QPalette::Window), QColor(24, 27, 40));
        QCOMPARE(qGuiApp->palette().color(QPalette::ButtonText), QColor(49, 181, 235));
        QCOMPARE(qGuiApp->palette().color(QPalette::Base), QColor(65, 70, 75));
    }

    void unlockButtonAndPanelUseUserPalette()
    {
        if (QGuiApplication::platformName() != QLatin1String("xcb")) {
            QSKIP("Rendered appearance test requires the private X11 wrapper");
        }
        ScreenLocker::GreeterAppearance appearance;
        QQmlApplicationEngine engine;
        appearance.setupEngine(&engine);
        engine.setProperty("_kirigamiTheme", QStringLiteral("KirigamiPlasmaStyle"));
        QCOMPARE(engine.property("_kirigamiTheme").toString(), QStringLiteral("org.kde.breeze"));
        KLocalization::setupLocalizedContext(&engine);
        engine.load(QUrl::fromLocalFile(QStringLiteral(APPEARANCE_TEST_QML)));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        // Unlike the old fixture, the SVG theme deliberately has its own
        // conflicting colors. Both regular controls and the original keyboard
        // must use the user's palette, without changing the shared keyboard.
        QCOMPARE(window->property("unlockTextColor").value<QColor>(), QColor(49, 181, 235));
        QCOMPARE(window->property("fieldBackgroundColor").value<QColor>(), QColor(65, 70, 75));
        const auto count = [window](const QColor &color, int top, int bottom) {
            const auto image = window->grabWindow();
            int matches = 0;
            for (int y = qMax(0, top); y < qMin(bottom, image.height()); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    matches += image.pixelColor(x, y) == color;
                }
            }
            return matches;
        };
        // Exercise an ordinary button before loading the keyboard/PlasmaCore.
        QTRY_VERIFY_WITH_TIMEOUT(count(QColor(240, 120, 0), 100, 160) > 5000, 5000);
        ScreenLocker::VirtualKeyboard controller;
        controller.addView(window, &engine);
        auto *panel = controller.panel();
        QVERIFY(panel);
        auto *editor = window->findChild<QQuickItem *>(QStringLiteral("password"));
        QVERIFY(editor);
        editor->forceActiveFocus();
        QTRY_COMPARE(QGuiApplication::focusObject(), editor);
        QGuiApplication::inputMethod()->show();
        QTRY_VERIFY(panel->isVisible());
        auto *keyboard = panel->property("keyboard").value<QObject *>();
        QVERIFY(keyboard);
        auto *style = keyboard->property("style").value<QObject *>();
        QVERIFY(style);
        auto *theme = style->property("theme").value<QObject *>();
        QVERIFY(theme);
        QCOMPARE(theme->property("keyboardBackgroundColor").value<QColor>(), QColor(24, 27, 40));
        QCOMPARE(theme->property("normalKeyBackgroundColor").value<QColor>(), QColor(24, 27, 40).lighter(130));
        QCOMPARE(theme->property("keyTextColor").value<QColor>(), QColor(102, 194, 242));
        QVERIFY(window->grabWindow().save(QStringLiteral(APPEARANCE_TEST_SCREENSHOT)));
        QTRY_VERIFY_WITH_TIMEOUT(count(QColor(24, 27, 40), 350, 700) > 1000, 5000);
        // Real keys, text pixels, and typing through the daemon's native grabs
        // are covered by virtualKeyboardLockTest in its independent client.
        QCOMPARE(QGuiApplication::focusObject(), editor);
        QGuiApplication::inputMethod()->hide();
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir home;
    if (!home.isValid()) {
        return 1;
    }
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toUtf8());
    qputenv("XDG_CONFIG_DIRS", (home.path() + QStringLiteral("/empty-config")).toUtf8());
    qputenv("XDG_DATA_HOME", (home.path() + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_CACHE_HOME", (home.path() + QStringLiteral("/cache")).toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/locker-appearance-test-bus");
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/locker-appearance-test-system-bus");
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("QT_QPA_PLATFORMTHEME", "kde");
    qputenv("QT_IM_MODULE", "sonicvirtualkeyboard");
    qputenv("QT_VIRTUALKEYBOARD_DESKTOP_DISABLE", "1");
    const auto layout = QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("plasma/keyboard/layouts/fallback/main.qml"));
    if (layout.isEmpty()) {
        qCritical("The complete Sonic Keyboard layout installation is required for this test");
        return 1;
    }
    qputenv("QT_VIRTUALKEYBOARD_LAYOUT_PATH", QDir::cleanPath(QFileInfo(layout).absolutePath() + QStringLiteral("/..")).toUtf8());
    qunsetenv("QT_VIRTUALKEYBOARD_STYLE");
    const auto write = [](const QString &path, const QByteArray &data) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            return false;
        }
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    };
    const auto desktop = home.path() + QStringLiteral("/data/plasma/desktoptheme/LockerAppearanceTest");
    QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\">";
    for (const auto &part : {"center", "left", "right", "top", "bottom", "topleft", "topright", "bottomleft", "bottomright"}) {
        svg += QByteArray("<rect id=\"normal-") + part + "\" width=\"24\" height=\"24\" fill=\"#f07800\"/>";
    }
    svg += "</svg>";
    if (!write(desktop + QStringLiteral("/metadata.json"), "{\"KPlugin\":{\"Id\":\"LockerAppearanceTest\",\"Name\":\"Locker appearance test\"}}")
        || !write(desktop + QStringLiteral("/widgets/button.svg"), svg)
        || !write(home.path() + QStringLiteral("/data/color-schemes/AppearanceDefault.colors"),
                  "[Colors:Window]\nBackgroundNormal=90,70,50\nForegroundNormal=255,255,255\n"
                  "[Colors:Button]\nBackgroundNormal=90,70,50\nForegroundNormal=255,255,255\n"
                  "[Colors:View]\nBackgroundNormal=65,70,75\nForegroundNormal=200,210,220\n")
        || !write(desktop + QStringLiteral("/colors"),
                  "[Colors:Window]\nBackgroundNormal=45,48,52\nForegroundNormal=255,255,255\n"
                  "[Colors:Button]\nBackgroundNormal=28,31,34\nForegroundNormal=255,255,255\n")
        || !write(home.path() + QStringLiteral("/config/plasmarc"), "[Theme]\nname=LockerAppearanceTest\n")
        || !write(home.path() + QStringLiteral("/config/kdeglobals"),
                  "[General]\nColorScheme=AppearanceDefault\n"
                  "[Colors:Window]\nBackgroundNormal=24,27,40\nForegroundNormal=102,194,242\n"
                  "[Colors:Button]\nBackgroundNormal=30,34,51\nForegroundNormal=49,181,235\n")) {
        return 1;
    }
    QGuiApplication app(argc, argv);
    ScreenLocker::GreeterAppearance appearance;
    GreeterAppearanceTest test;
    int result = 1;
    QTimer::singleShot(0, &app, [&] {
        result = QTest::qExec(&test, argc, argv);
        app.exit(result);
    });
    app.exec();
    return result;
}

#include "greeterappearancetest.moc"
