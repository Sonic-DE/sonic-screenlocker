/*
 * SPDX-FileCopyrightText: 2026 SonicDE Community
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../greeter/virtualkeyboard.h"
#include "../x11locker.h"

#include <KLocalizedQmlContext>
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QGuiApplication>
#include <QInputMethod>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QScreen>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>
#include <QWidget>
#include <private/qguiapplication_p.h>
#include <private/qtx11extras_p.h>
#include <qpa/qplatforminputcontext.h>
#include <qpa/qplatformintegration.h>

#include <functional>
#include <unistd.h>

#include <X11/Xlib.h>
#include <fixx11h.h>
#include <xcb/xtest.h>

class KeyboardClient : public QObject {
    Q_OBJECT
public:
    QQuickWindow *window = nullptr;
    QObject *editor = nullptr;
    ScreenLocker::VirtualKeyboard *keyboard = nullptr;

    static QQuickItem *footerButton(QQuickItem *item)
    {
        if (item->isVisible() && item->isEnabled() && item->property("text").toString() == QLatin1String("Virtual Keyboard")
            && item->metaObject()->indexOfSignal("clicked()") >= 0) {
            return item;
        }
        for (auto *child : item->childItems()) {
            if (auto *button = footerButton(child)) {
                return button;
            }
        }
        return nullptr;
    }

public Q_SLOTS:
    void report()
    {
        const auto *inputContext = QGuiApplicationPrivate::platformIntegration()->inputContext();
        QJsonObject message{
            {QStringLiteral("window"), qint64(window->winId())},
            {QStringLiteral("display"), qEnvironmentVariable("DISPLAY")},
            {QStringLiteral("windowHeight"), window->height()},
            {QStringLiteral("windowWidth"), window->width()},
            {QStringLiteral("module"), qEnvironmentVariable("QT_IM_MODULE")},
            {QStringLiteral("pluginPaths"), QCoreApplication::libraryPaths().join(QLatin1Char(':'))},
            {QStringLiteral("backend"), inputContext ? QString::fromLatin1(inputContext->metaObject()->className()) : QStringLiteral("unavailable")},
            {QStringLiteral("visible"), QGuiApplication::inputMethod()->isVisible()},
            {QStringLiteral("text"), editor->property("text").toString()},
            {QStringLiteral("masked"), editor->property("displayText").toString()},
            {QStringLiteral("focused"), QGuiApplication::focusObject() == editor},
            {QStringLiteral("footerActivations"), window->property("footerActivations").toInt()},
            {QStringLiteral("embedded"), keyboard->panel() && keyboard->panel()->window() == window}};
        if (auto *panel = keyboard->panel()) {
            message.insert(QStringLiteral("style"), panel->property("keyboardStyleName").toString());
            message.insert(QStringLiteral("panelWidth"), panel->width());
            message.insert(QStringLiteral("panelHeight"), panel->height());
            message.insert(QStringLiteral("panelY"), panel->y());
            std::function<QQuickItem *(QQuickItem *)> findKey = [&](QQuickItem *item) -> QQuickItem * {
                if (item->isVisible() && item->isEnabled() && item->property("key").isValid()
                    && item->property("text").toString().toLower() == QLatin1String("q")) {
                    return item;
                }
                for (auto *child : item->childItems()) {
                    if (auto *key = findKey(child)) {
                        return key;
                    }
                }
                return nullptr;
            };
            if (auto *key = findKey(panel)) {
                const auto point = window->mapToGlobal(key->mapToScene(QPointF(key->width() / 2, key->height() / 2)).toPoint());
                message.insert(QStringLiteral("keyX"), point.x());
                message.insert(QStringLiteral("keyY"), point.y());
            }
        }
        message.insert(QStringLiteral("footerAvailable"), footerButton(window->contentItem()) != nullptr);
        QTextStream out(stdout);
        out << QJsonDocument(message).toJson(QJsonDocument::Compact) << '\n';
        out.flush();
    }
};

static int runClient(int argc, char **argv)
{
    qputenv("QT_IM_MODULE", "sonicvirtualkeyboard");
    qputenv("QT_VIRTUALKEYBOARD_DESKTOP_DISABLE", "1");
    QGuiApplication app(argc, argv);
    // Match UnlockApp: the daemon owns X11 grabs and sends logical FocusIn to
    // the registered greeter; NotifyGrab FocusOut must not deactivate Qt input.
    class KeepGreeterFocus : public QAbstractNativeEventFilter {
        bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override
        {
            if (type != QByteArrayLiteral("xcb_generic_event_t")) {
                return false;
            }
            return (static_cast<xcb_generic_event_t *>(message)->response_type & ~0x80) == XCB_FOCUS_OUT;
        }
    } focusFilter;
    app.installNativeEventFilter(&focusFilter);
    QQmlApplicationEngine engine;
    KLocalization::setupLocalizedContext(&engine);
    ScreenLocker::VirtualKeyboard keyboard;
    engine.load(QUrl::fromLocalFile(QStringLiteral(KEYBOARD_CLIENT_QML)));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    auto *editor = window->findChild<QQuickItem *>(QStringLiteral("password"));
    if (!editor) {
        return 1;
    }
    keyboard.addView(window, &engine);
    KeyboardClient status;
    status.window = window;
    status.editor = editor;
    status.keyboard = &keyboard;
    QObject::connect(editor, SIGNAL(textChanged()), &status, SLOT(report()));
    QObject::connect(QGuiApplication::inputMethod(), &QInputMethod::visibleChanged, &status, &KeyboardClient::report);
    QObject::connect(QGuiApplication::inputMethod(), &QInputMethod::keyboardRectangleChanged, &status, &KeyboardClient::report);
    // Layout and style loading can complete after input-method notifications.
    QTimer reportTimer;
    QObject::connect(&reportTimer, &QTimer::timeout, &status, &KeyboardClient::report);
    reportTimer.start(100);
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
        char buffer[128];
        const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        const QByteArray command(buffer, qMax<qint64>(0, count));
        if (command.contains("quit")) {
            app.quit();
        } else if (command.contains("activateFooter")) {
            if (auto *button = KeyboardClient::footerButton(window->contentItem())) {
                if (QGuiApplication::inputMethod()->isVisible()) {
                    QMetaObject::invokeMethod(button, "clicked");
                } else {
                    const auto point = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
                    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
                }
            }
        }
        QTimer::singleShot(100, &status, &KeyboardClient::report);
    });
    window->requestActivate();
    editor->forceActiveFocus();
    QTimer::singleShot(100, &status, &KeyboardClient::report);
    return app.exec();
}

class VirtualKeyboardLockTest : public QObject {
    Q_OBJECT
    static QImage rootImage()
    {
        return QGuiApplication::primaryScreen()->grabWindow(QX11Info::appRootWindow(), 0, 0, 1920, 1080).toImage();
    }

private Q_SLOTS:
    void keyboardStaysInsideTrustedSurface()
    {
        if (qgetenv("SCREENLOCKER_KEYBOARD_TEST_ISOLATED") != "1") {
            QSKIP("This mutating X11 test requires its private Xvfb wrapper");
        }
        ScreenLocker::X11Locker locker;
        locker.showLockWindow();
        const auto unlock = qScopeGuard([&] {
            locker.hideLockWindow();
        });
        QProcess client;
        auto clientEnvironment = QProcessEnvironment::systemEnvironment();
        clientEnvironment.insert(QStringLiteral("QT_IM_MODULE"), QStringLiteral("sonicvirtualkeyboard"));
        clientEnvironment.insert(QStringLiteral("QT_VIRTUALKEYBOARD_DESKTOP_DISABLE"), QStringLiteral("1"));
        client.setProcessEnvironment(clientEnvironment);
        client.setProcessChannelMode(QProcess::SeparateChannels);
        QJsonObject state;
        QObject::connect(&client, &QProcess::readyReadStandardOutput, &client, [&] {
            while (client.canReadLine()) {
                const auto message = QJsonDocument::fromJson(client.readLine());
                if (message.isObject()) {
                    state = message.object();
                }
            }
        });
        client.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--client")});
        QVERIFY(client.waitForStarted());
        const auto stopClient = qScopeGuard([&] {
            if (client.state() == QProcess::NotRunning) {
                return;
            }
            client.write("quit\n");
            if (!client.waitForFinished(5000)) {
                client.kill();
                client.waitForFinished();
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(state.contains(QStringLiteral("window")) || client.state() == QProcess::NotRunning, 10000);
        QVERIFY2(state.contains(QStringLiteral("window")), client.readAllStandardError().constData());
        const auto greeter = WId(state.value(QStringLiteral("window")).toDouble());
        locker.addAllowedWindow(greeter);
        QVERIFY(state.value(QStringLiteral("embedded")).toBool());
        QVERIFY2(state.value(QStringLiteral("backend")).toString().contains(QStringLiteral("SonicQtVirtualKeyboard")),
                 QByteArray(QJsonDocument(state).toJson(QJsonDocument::Compact) + client.readAllStandardError()).constData());

        Display *display = QX11Info::display();
        const auto root = QX11Info::appRootWindow();
        QCOMPARE(QSize(DisplayWidth(display, DefaultScreen(display)), DisplayHeight(display, DefaultScreen(display))), QSize(1920, 1080));
        QCOMPARE(state.value(QStringLiteral("windowHeight")).toInt(), 1080);
        QCOMPARE(state.value(QStringLiteral("windowWidth")).toInt(), 1920);
        QCOMPARE(rootImage().size(), QSize(1920, 1080));
        QCOMPARE(XGrabKeyboard(display, root, false, GrabModeAsync, GrabModeAsync, CurrentTime), GrabSuccess);
        QCOMPARE(XGrabPointer(display, root, false, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, 0, 0, CurrentTime),
                 GrabSuccess);
        const auto releaseGrabs = qScopeGuard([&] {
            XUngrabKeyboard(display, CurrentTime);
            XUngrabPointer(display, CurrentTime);
            XSync(display, false);
        });
        QTRY_VERIFY(state.value(QStringLiteral("footerAvailable")).toBool());
        // Click the actual QML footer button in the client. Key input below
        // traverses the daemon's native X11 grab/routing path as well.
        client.write("activateFooter\n");
        QTRY_VERIFY2_WITH_TIMEOUT(state.value(QStringLiteral("visible")).toBool() && state.contains(QStringLiteral("keyX")),
                                  QByteArray(QJsonDocument(state).toJson(QJsonDocument::Compact) + client.readAllStandardError()).constData(),
                                  5000);
        QCOMPARE(state.value(QStringLiteral("footerActivations")).toInt(), 1);
        QCOMPARE(state.value(QStringLiteral("style")).toString(), QStringLiteral("Breeze"));
        auto keyboardPixels = [&] {
            const auto image = rootImage();
            int different = 0;
            for (int y = image.height() * 2 / 3; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const auto color = image.pixelColor(x, y);
                    if (color != QColor(QStringLiteral("#803040")) && color != QColor(Qt::black)) {
                        ++different;
                    }
                }
            }
            return different;
        };
        QTRY_VERIFY2_WITH_TIMEOUT(keyboardPixels() > 10000,
                                  QByteArray(QJsonDocument(state).toJson(QJsonDocument::Compact) + client.readAllStandardError()).constData(),
                                  5000);
        QWidget rogue;
        rogue.setWindowFlags(Qt::Window | Qt::X11BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus);
        rogue.setAutoFillBackground(true);
        auto palette = rogue.palette();
        palette.setColor(QPalette::Window, Qt::green);
        rogue.setPalette(palette);
        rogue.setGeometry(0, 0, 1920, 1080);
        rogue.show();
        rogue.raise();
        XSync(display, false);
        QTRY_VERIFY_WITH_TIMEOUT(rootImage().pixelColor(10, 10) == QColor(QStringLiteral("#803040")), 5000);
        QTRY_VERIFY(keyboardPixels() > 10000);

        const auto x = state.value(QStringLiteral("keyX")).toInt();
        const auto y = state.value(QStringLiteral("keyY")).toInt();
        auto *connection = QX11Info::connection();
        xcb_test_fake_input(connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root, x, y, 0);
        xcb_test_fake_input(connection, XCB_BUTTON_PRESS, 1, XCB_CURRENT_TIME, root, 0, 0, 0);
        xcb_test_fake_input(connection, XCB_BUTTON_RELEASE, 1, XCB_CURRENT_TIME, root, 0, 0, 0);
        xcb_flush(connection);
        QTRY_COMPARE_WITH_TIMEOUT(state.value(QStringLiteral("text")).toString().toLower(), QStringLiteral("q"), 5000);
        QVERIFY(state.value(QStringLiteral("focused")).toBool());
        QTRY_VERIFY(!state.value(QStringLiteral("masked")).toString().isEmpty()
                    && state.value(QStringLiteral("masked")).toString().toLower() != QLatin1String("q"));
        client.write("activateFooter\n");
        QTRY_VERIFY(!state.value(QStringLiteral("visible")).toBool());
        rogue.hide();
        client.write("quit\n");
        QVERIFY(client.waitForFinished(5000));
        QCOMPARE(client.exitStatus(), QProcess::NormalExit);
        QCOMPARE(client.exitCode(), 0);
        const auto warnings = client.readAllStandardError();
        QVERIFY2(!warnings.contains("TypeError") && !warnings.contains("ReferenceError"), warnings.constData());
        if (!warnings.isEmpty()) {
            qWarning().noquote() << warnings;
        }
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
    qputenv("XDG_CACHE_HOME", (home.path() + QStringLiteral("/cache")).toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/screenlocker-keyboard-test-bus");
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/screenlocker-keyboard-test-system-bus");
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    qputenv("QT_SCALE_FACTOR", "1");
    qputenv("QT_SCREEN_SCALE_FACTORS", "1");
    qputenv("LANGUAGE", "C");
    if (argc > 1 && QByteArray(argv[1]) == "--client") {
        return runClient(argc, argv);
    }
    qputenv("QT_IM_MODULE", "compose");
    QApplication app(argc, argv);
    VirtualKeyboardLockTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "virtualkeyboardtest.moc"
