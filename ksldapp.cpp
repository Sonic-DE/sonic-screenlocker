/*
    SPDX-FileCopyrightText: 1999 Martin R. Jones <mjones@kde.org>
    SPDX-FileCopyrightText: 2003 Oswald Buddenhagen <ossi@kde.org>
    SPDX-FileCopyrightText: 2008 Chani Armitage <chanika@gmail.com>
    SPDX-FileCopyrightText: 2011 Martin Gräßlin <mgraesslin@kde.org>

SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "ksldapp.h"
#include "globalaccel.h"
#include "greeteripcserver.h"
#include "interface.h"
#include "kscreensaversettings.h"
#include "logind.h"
#include "powermanagement_inhibition.h"
#include "x11locker.h"

#include <config-kscreenlocker.h>

#include <Sonic/Logging/Logging>

// KSldApp runs inside the compositor, so the compositor's executable names the
// log file; the automatic install leaves a handler the compositor installed alone.
SONIC_LOGGING_COMPONENT("org.sonicde.screenlocker")
SONIC_LOGGING_AUTO_INSTALL()

#include "config-unix.h"
// KDE
#include <KAuthorized>
#include <KGlobalAccel>
#include <KIdleTime>
#include <KLibexec>
#include <KLocalizedString>
#include <KNotification>

// Qt
#include <QAction>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QFile>
#include <QKeyEvent>
#include <QProcess>
#include <QTimer>
// X11
#include "x11info.h"
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <xcb/xcb.h>
// other
#include <signal.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

namespace ScreenLocker
{

QString establishLockToString(EstablishLock establishLock)
{
    switch (establishLock) {
    case EstablishLock::Immediate:
        return QStringLiteral("Immediate");
    case EstablishLock::Delayed:
        return QStringLiteral("Delayed");
    }
    Q_UNREACHABLE();
}

static const QString s_qtQuickBackend = QStringLiteral("QT_QUICK_BACKEND");

static KSldApp *s_instance = nullptr;

KSldApp *KSldApp::self()
{
    if (!s_instance) {
        s_instance = new KSldApp();
    }

    return s_instance;
}

KSldApp::KSldApp(QObject *parent)
    : QObject(parent)
    , m_lockState(Unlocked)
    , m_lockProcess(nullptr)
    , m_lockWindow(nullptr)
    , m_lockedTimer(QElapsedTimer())
    , m_idleId(0)
    , m_lockGrace(0)
    , m_inGraceTime(false)
    , m_graceTimer(new QTimer(this))
    , m_inhibitCounter(0)
    , m_logind(nullptr)
    , m_greeterEnv(QProcessEnvironment::systemEnvironment())
    , m_powerManagementInhibition(new PowerManagementInhibition(this))
{
}

KSldApp::~KSldApp()
{
}

static int s_XTimeout;
static int s_XInterval;
static int s_XBlanking;
static int s_XExposures;

void KSldApp::cleanUp()
{
    sonicDebug() << "Cleaning up";

    if (m_lockProcess && m_lockProcess->state() != QProcess::NotRunning) {
        sonicDebug() << "Terminating lock process";
        m_lockProcess->terminate();
    }
    delete m_lockProcess;
    delete m_lockWindow;

    // Restore X screensaver parameters
    XSetScreenSaver(X11Info::display(), s_XTimeout, s_XInterval, s_XBlanking, s_XExposures);
}

static bool s_graceTimeKill = false;
static bool s_logindExit = false;
static bool s_lockProcessRequestedExit = false;

static bool hasXInput()
{
    Display *dpy = X11Info::display();
    int xi_opcode, event, error;
    // init XInput extension
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &event, &error)) {
        return false;
    }

    // verify that the XInput extension is at at least version 2.0
    int major = 2, minor = 0;
    int result = XIQueryVersion(dpy, &major, &minor);
    if (result == BadImplementation) {
        // Xinput 2.2 returns BadImplementation if checked against 2.0
        major = 2;
        minor = 2;
        return XIQueryVersion(dpy, &major, &minor) == Success;
    }
    return result == Success;
}

void KSldApp::initializeX11()
{
    sonicDebug() << "Initializing X11";

    m_hasXInput2 = hasXInput();
    // Save X screensaver parameters
    XGetScreenSaver(X11Info::display(), &s_XTimeout, &s_XInterval, &s_XBlanking, &s_XExposures);
    // And disable it. The internal X screensaver is not used at all, but we use its
    // internal idle timer (and it is also used by DPMS support in X). This timer must not
    // be altered by this code, since e.g. resetting the counter after activating our
    // screensaver would prevent DPMS from activating. We use the timer merely to detect
    // user activity.
    XSetScreenSaver(X11Info::display(), 0, s_XInterval, s_XBlanking, s_XExposures);
}

void KSldApp::initialize()
{
    sonicDebug() << "Initializing";

    m_requirePassword = KScreenSaverSettings::requirePassword();

    initializeX11();

    // Initialize custom IPC server for greeter communication
    m_ipcServer = new GreeterIpcServer(this);
    if (!m_ipcServer->initialize()) {
        sonicWarning() << "Failed to initialize IPC server";
        delete m_ipcServer;
        m_ipcServer = nullptr;
    } else {
        // Connect IPC server signals to KSldApp slots
        connect(m_ipcServer, &GreeterIpcServer::greeterWindowRegistered, this, &KSldApp::registerGreeterWindow);
        connect(m_ipcServer, &GreeterIpcServer::greeterWindowUnregistered, this, &KSldApp::unregisterGreeterWindow);
        connect(m_ipcServer, &GreeterIpcServer::greeterAuthenticationSuccess, this, &KSldApp::greeterAuthenticationSuccess);
        connect(m_ipcServer, &GreeterIpcServer::greeterGetFocusRequested, this, &KSldApp::greeterGetFocus);

        // Set environment variable for greeter to find the IPC socket
        m_greeterEnv.insert(QStringLiteral("KSCREENLOCKER_IPC_SOCKET"), GreeterIpcServer::socketName());
    }

    // Global keys - only register if kglobalaccel service is available
    if (KAuthorized::authorizeAction(QStringLiteral("lock_screen"))) {
        // Check if kglobalaccel service is available
        QDBusInterface kglobalaccelInterface(QStringLiteral("org.kde.kglobalaccel"),
                                             QStringLiteral("/kglobalaccel"),
                                             QStringLiteral("org.kde.KGlobalAccel"),
                                             QDBusConnection::sessionBus());
        bool kglobalaccelAvailable = kglobalaccelInterface.isValid();

        QAction *a = new QAction(this);
        a->setObjectName(QStringLiteral("Lock Session"));
        // The following properties are set manually because we do not depend
        // on KXmlGui and therefore cannot use KActionCollection.
        a->setProperty("componentDisplayName", i18nc("Name of a category in System Settings' Shortcuts KCM; match it exactly", "Session Management"));
        a->setProperty("componentName", QStringLiteral("ksmserver"));
        a->setText(i18n("Lock Session"));

        if (kglobalaccelAvailable) {
            KGlobalAccel::self()->setGlobalShortcut(a, KScreenSaverSettings::defaultShortcuts());
        }

        connect(a, &QAction::triggered, this, [this]() {
            sonicDebug() << "Locking session due to global shortcut";
            const EstablishLock lockType = m_requirePassword ? EstablishLock::Immediate : EstablishLock::Delayed;
            m_inGraceTime = !m_requirePassword;
            if (m_inGraceTime) {
                m_lockGrace = -1;
            }
            lock(lockType);
        });
    }

    connect(m_powerManagementInhibition, &PowerManagementInhibition::inhibitedChanged, this, &KSldApp::updateIdleTimeout);

    // idle support
    auto idleTimeSignal = static_cast<void (KIdleTime::*)(int, int)>(&KIdleTime::timeoutReached);
    connect(KIdleTime::instance(), idleTimeSignal, this, [this](int identifier) {
        if (identifier != m_idleId) {
            // not our identifier
            return;
        }
        if (lockState() != Unlocked) {
            return;
        }
        if (isInhibited()) {
            // either we got a direct inhibit request thru our outdated o.f.Screensaver iface ...
            // ... or the newer one at o.f.PowerManagement.Inhibit
            // there is at least one process blocking the auto lock of screen locker
            return;
        }
        if (m_lockGrace) { // short-circuit if grace time is zero
            m_inGraceTime = true;
        }

        sonicDebug() << "Idle timeout reached. Locking now.";
        lock(EstablishLock::Delayed);
    });

    m_lockProcess = new QProcess();
    m_lockProcess->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    auto finishedSignal = static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished);
    connect(m_lockProcess, finishedSignal, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        sonicDebug() << "Greeter process exitted with status:" << exitStatus << "exit code:" << exitCode;

        if (s_lockProcessRequestedExit) {
            sonicDebug() << "Lock process had requested exit; we already did the unlock process elsewhere";
            return;
        }

        if (s_graceTimeKill) {
            sonicDebug() << "screen locker killed due to grace time.";
            s_graceTimeKill = false;
            s_logindExit = false;
            return;
        }
        const bool regularExit = !exitCode && exitStatus == QProcess::NormalExit;
        if (regularExit || s_logindExit) {
            // unlock process finished successfully - we can remove the lock grab

            if (regularExit) {
                sonicDebug() << "Unlocking now on regular exit.";
            } else {
                Q_ASSERT(s_logindExit);
                sonicDebug() << "Unlocking anyway since forced through logind.";
            }

            s_graceTimeKill = false;
            s_logindExit = false;
            doUnlock();
            return;
        }

        sonicWarning() << "Greeter process exit unregular. Restarting lock.";

        m_greeterCrashedCounter++;
        if (m_greeterCrashedCounter < 4) {
            // Perhaps it crashed due to a graphics driver issue, force software rendering now
            sonicDebug().nospace() << "Trying to lock again with software rendering (" << m_greeterCrashedCounter << "/4).";
            setForceSoftwareRendering(true);
            startLockProcess(EstablishLock::Immediate);
        } else if (m_lockWindow) {
            sonicWarning() << "Everything else failed. Need to put Greeter in emergency mode.";
            m_lockWindow->emergencyShow();
        } else {
            sonicCritical() << "Greeter process exitted and we could in no way recover from that!";
        }
    });
    connect(m_lockProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            sonicDebug() << "Greeter Process  failed to start. Trying to directly unlock again.";
            doUnlock();
            sonicCritical() << "Greeter Process not available";
        } else {
            sonicWarning() << "Greeter Process encountered an unhandled error:" << error << ". Detailed error:" << m_lockProcess->errorString();
        }
    });
    m_lockedTimer.invalidate();
    m_graceTimer->setSingleShot(true);
    connect(m_graceTimer, &QTimer::timeout, this, &KSldApp::endGraceTime);
    // create our D-Bus interface
    new Interface(this);

    // connect to logind
    m_logind = new LogindIntegration(this);
    connect(m_logind, &LogindIntegration::requestLock, this, [this]() {
        sonicDebug() << "Lock requested by logind";
        lock(EstablishLock::Immediate);
    });
    connect(m_logind, &LogindIntegration::requestUnlock, this, [this]() {
        if (lockState() == Locked || lockState() == AcquiringLock) {
            if (m_lockProcess->state() != QProcess::NotRunning) {
                s_logindExit = true;
                m_lockProcess->terminate();
            } else {
                doUnlock();
            }
        }
    });
    connect(m_logind, &LogindIntegration::prepareForSleep, this, [this](bool goingToSleep) {
        if (!goingToSleep) {
            // not interested in doing anything on wakeup
            sonicDebug() << "Prepare for sleep called, but not going to sleep";
            return;
        }
        if (KScreenSaverSettings::lockOnResume()) {
            sonicDebug() << "Prepare for sleep called, locking now";
            lock(EstablishLock::Immediate);
        }
    });
    connect(m_logind, &LogindIntegration::inhibited, this, [this]() {
        // if we are already locked, we immediately remove the inhibition lock
        if (m_lockState == KSldApp::Locked) {
            m_logind->uninhibit();
        }
    });
    connect(m_logind, &LogindIntegration::connectedChanged, this, [this]() {
        if (!m_logind->isConnected()) {
            return;
        }
        if (m_lockState == ScreenLocker::KSldApp::Unlocked && KScreenSaverSettings::lockOnResume()) {
            m_logind->inhibit();
        }
        if (m_logind->isLocked()) {
            sonicDebug() << "LogindIntegration::connectedChanged signal received, locking now";
            lock(EstablishLock::Immediate);
        }
    });

    m_globalAccel = new GlobalAccel(this);

    // fallback for non-logind systems:
    // connect to signal emitted by Solid. This is emitted unconditionally also on logind enabled systems
    // ksld ignores it in case logind is used
    QDBusConnection::sessionBus().connect(QStringLiteral("org.kde.Solid.PowerManagement"),
                                          QStringLiteral("/org/kde/Solid/PowerManagement/Actions/SuspendSession"),
                                          QStringLiteral("org.kde.Solid.PowerManagement.Actions.SuspendSession"),
                                          QStringLiteral("aboutToSuspend"),
                                          this,
                                          SLOT(solidSuspend()));

    configure();

    if (m_logind->isLocked()) {
        sonicDebug() << "Logind is locked, locking now";
        lock(EstablishLock::Immediate);
    }
    if (KScreenSaverSettings::lockOnStart()) {
        sonicDebug() << "Lock on start enabled, locking now";
        lock(EstablishLock::Immediate);
    }
}

void KSldApp::lockProcessRequestedUnlock()
{
    sonicDebug() << "Lock process requested unlock";

    s_graceTimeKill = false;
    s_logindExit = false;
    s_lockProcessRequestedExit = true;
    doUnlock();
}

void KSldApp::registerGreeterWindow(uint winId, const QString &screenName)
{
    // Store the window ID for later grab management
    m_greeterWindows.insert(winId, screenName);
    // Tell the lock window to allow this window
    if (m_lockWindow) {
        m_lockWindow->addAllowedWindow(winId);
    }
}

void KSldApp::unregisterGreeterWindow(uint winId)
{
    m_greeterWindows.remove(winId);
}

void KSldApp::greeterAuthenticationSuccess()
{
    lockProcessRequestedUnlock();
}

void KSldApp::greeterGetFocus(const QString &screenName)
{
    // Focus is managed by the greeter itself, but we log it here
    Q_UNUSED(screenName);
}

void KSldApp::configure()
{
    sonicDebug() << "Configuring";

    KScreenSaverSettings::self()->load();
    // idle support
    if (m_idleId) {
        KIdleTime::instance()->removeIdleTimeout(m_idleId);
        m_idleId = 0;
    }
    updateIdleTimeout();
    if (KScreenSaverSettings::lock()) {
        // lockGrace is stored in seconds
        m_lockGrace = KScreenSaverSettings::lockGrace() * 1000;
    } else {
        m_lockGrace = -1;
    }
    if (m_logind && m_logind->isConnected()) {
        if (KScreenSaverSettings::lockOnResume() && !m_logind->isInhibited()) {
            m_logind->inhibit();
        } else if (!KScreenSaverSettings::lockOnResume() && m_logind->isInhibited()) {
            m_logind->uninhibit();
        }
    }
    m_requirePassword = KScreenSaverSettings::requirePassword();
}

void KSldApp::lock(EstablishLock establishLock, int attemptCount)
{
    sonicDebug() << "lock called with establishLock:" << establishLockToString(establishLock) << "attemptCount:" << attemptCount;

    if (lockState() != Unlocked) {
        // already locked or acquiring lock, no need to lock again
        // but make sure it's really locked
        endGraceTime();
        if (establishLock == EstablishLock::Immediate) {
            // signal the greeter to switch to immediateLock mode
            kill(m_lockProcess->processId(), SIGUSR1);
        }
        return;
    }

    if (attemptCount == 0) {
        Q_EMIT aboutToLock();
    }

    if (!establishGrab()) {
        if (attemptCount < 3) {
            sonicWarning() << "Could not establish screen lock. Trying again in 10ms";
            QTimer::singleShot(10, this, [this, establishLock, attemptCount]() {
                lock(establishLock, attemptCount + 1);
            });
        } else {
            sonicCritical() << "Could not establish screen lock";
            Q_EMIT lockFailed();
        }
        return;
    }

    KNotification::event(QStringLiteral("locked"), i18n("Screen locked"), QPixmap(), KNotification::CloseOnTimeout, QStringLiteral("ksmserver"));

    s_lockProcessRequestedExit = false;

    // blank the screen
    showLockWindow();

    m_lockState = AcquiringLock;

    setForceSoftwareRendering(false);
    // start unlock screen process
    startLockProcess(establishLock);
    Q_EMIT lockStateChanged();
}

/*
 * Forward declarations:
 * Only called from KSldApp::establishGrab(). Using from somewhere else is incorrect usage!
 **/
static bool grabKeyboard();
static bool grabMouse();

class XServerGrabber
{
public:
    XServerGrabber()
    {
        xcb_grab_server(X11Info::connection());
    }
    ~XServerGrabber()
    {
        xcb_ungrab_server(X11Info::connection());
        xcb_flush(X11Info::connection());
    }
};

bool KSldApp::establishGrab()
{
    sonicDebug() << "Establishing grab";

    XSync(X11Info::display(), False);
    XServerGrabber serverGrabber;
    if (!grabKeyboard()) {
        return false;
    }

    if (!grabMouse()) {
        XUngrabKeyboard(X11Info::display(), CurrentTime);
        XFlush(X11Info::display());
        return false;
    }

    if (m_hasXInput2) {
        // get all devices
        Display *dpy = X11Info::display();
        int numMasters;
        XIDeviceInfo *masters = XIQueryDevice(dpy, XIAllMasterDevices, &numMasters);
        bool success = true;
        for (int i = 0; i < numMasters; ++i) {
            // ignoring core pointer and core keyboard as we already grabbed them
            if (qstrcmp(masters[i].name, "Virtual core pointer") == 0) {
                continue;
            }
            if (qstrcmp(masters[i].name, "Virtual core keyboard") == 0) {
                continue;
            }
            XIEventMask mask;
            uchar bitmask[] = {0, 0};
            mask.deviceid = masters[i].deviceid;
            mask.mask = bitmask;
            mask.mask_len = sizeof(bitmask);
            XISetMask(bitmask, XI_ButtonPress);
            XISetMask(bitmask, XI_ButtonRelease);
            XISetMask(bitmask, XI_Motion);
            XISetMask(bitmask, XI_Enter);
            XISetMask(bitmask, XI_Leave);
            const int result = XIGrabDevice(dpy,
                                            masters[i].deviceid,
                                            X11Info::appRootWindow(),
                                            XCB_TIME_CURRENT_TIME,
                                            XCB_CURSOR_NONE,
                                            XIGrabModeAsync,
                                            XIGrabModeAsync,
                                            True,
                                            &mask);
            if (result != XIGrabSuccess) {
                success = false;
                break;
            }
        }
        if (!success) {
            // ungrab all devices again
            for (int i = 0; i < numMasters; ++i) {
                XIUngrabDevice(dpy, masters[i].deviceid, XCB_TIME_CURRENT_TIME);
            }
            xcb_connection_t *c = X11Info::connection();
            xcb_ungrab_keyboard(c, XCB_CURRENT_TIME);
            xcb_ungrab_pointer(c, XCB_CURRENT_TIME);
        }
        XIFreeDeviceInfo(masters);
        XFlush(dpy);
        return success;
    }

    return true;
}

static bool grabKeyboard()
{
    sonicDebug() << "Grabbing keyboard";

    int rv = XGrabKeyboard(X11Info::display(), X11Info::appRootWindow(), True, GrabModeAsync, GrabModeAsync, CurrentTime);

    return (rv == GrabSuccess);
}

static bool grabMouse()
{
    sonicDebug() << "Grabbing mouse";

#define GRABEVENTS ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask
    int rv = XGrabPointer(X11Info::display(), X11Info::appRootWindow(), True, GRABEVENTS, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
#undef GRABEVENTS

    return (rv == GrabSuccess);
}

void KSldApp::doUnlock()
{
    sonicDebug() << "Unlocking now.";

    xcb_connection_t *c = X11Info::connection();
    xcb_ungrab_keyboard(c, XCB_CURRENT_TIME);
    xcb_ungrab_pointer(c, XCB_CURRENT_TIME);
    xcb_flush(c);
    if (m_hasXInput2) {
        // get all devices
        Display *dpy = X11Info::display();
        int numMasters;
        XIDeviceInfo *masters = XIQueryDevice(dpy, XIAllMasterDevices, &numMasters);
        // ungrab all devices again
        for (int i = 0; i < numMasters; ++i) {
            XIUngrabDevice(dpy, masters[i].deviceid, XCB_TIME_CURRENT_TIME);
        }
        XIFreeDeviceInfo(masters);
        XFlush(dpy);
    }
    hideLockWindow();
    // delete the window again, to get rid of event filter
    delete m_lockWindow;
    m_lockWindow = nullptr;
    m_lockState = Unlocked;
    m_lockedTimer.invalidate();
    m_greeterCrashedCounter = 0;
    endGraceTime();
    KNotification::event(QStringLiteral("unlocked"), i18n("Screen unlocked"), QPixmap(), KNotification::CloseOnTimeout, QStringLiteral("ksmserver"));
    if (m_logind) {
        m_logind->setLocked(false);
        if (KScreenSaverSettings::lockOnResume()) {
            m_logind->inhibit();
        }
    }
    if (m_globalAccel) {
        m_globalAccel->release();
    }
    Q_EMIT unlocked();
    Q_EMIT lockStateChanged();
}

bool KSldApp::isInhibited() const
{
    return m_inhibitCounter || m_powerManagementInhibition->isInhibited();
}

void KSldApp::startLockProcess(EstablishLock establishLock)
{
    sonicDebug() << "Starting lock process with establishLock:" << establishLockToString(establishLock);

    Q_EMIT aboutToStartGreeter();

    QProcessEnvironment env = m_greeterEnv;

    QStringList args;
    if (establishLock == EstablishLock::Immediate) {
        args << QStringLiteral("--immediateLock");
    }

    if (m_lockGrace > 0) {
        args << QStringLiteral("--graceTime");
        args << QString::number(m_lockGrace);
    }
    if (m_lockGrace == -1 || !m_requirePassword) {
        args << QStringLiteral("--nolock");
    }
    if (m_forceSoftwareRendering) {
        env.insert(s_qtQuickBackend, QStringLiteral("software"));
    }

    auto greeterPath = KLibexec::path(QStringLiteral(KSCREENLOCKER_GREET_BIN_REL));
    if (!QFile::exists(greeterPath)) {
        greeterPath = QStringLiteral(KSCREENLOCKER_GREET_BIN_ABS);
    }

    sonicDebug() << "Starting greeter process. Args:" << args;

    m_lockProcess->setProcessEnvironment(env);
    m_lockProcess->start(greeterPath, args);
}

void KSldApp::userActivity()
{
    if (m_lockState != Locked) {
        return;
    }

    sonicDebug() << "User activity detected. lockState:" << m_lockState;

    if (isGraceTime() || !m_requirePassword) {
        unlock();
    }
    if (m_lockWindow) {
        Q_EMIT m_lockWindow->userActivity();
    }
}

void KSldApp::showLockWindow()
{
    sonicDebug() << "Showing lock window";

    if (!m_lockWindow) {
        sonicDebug() << "Creating lock window";

        {
            m_lockWindow = new X11Locker(this);

            connect(
                m_lockWindow,
                &AbstractLocker::userActivity,
                m_lockWindow,
                [this]() {
                    if (isGraceTime() || !m_requirePassword) {
                        unlock();
                    }
                },
                Qt::QueuedConnection);
        }

        if (!m_lockWindow) {
            return;
        }
        m_lockWindow->setGlobalAccel(m_globalAccel);

        connect(m_lockWindow, &AbstractLocker::lockWindowShown, this, &KSldApp::lockScreenShown);
    }
    m_lockWindow->showLockWindow();
    XSync(X11Info::display(), False);
}

void KSldApp::hideLockWindow()
{
    if (!m_lockWindow) {
        sonicDebug() << "No lock window to hide";
        return;
    }

    sonicDebug() << "Hiding lock window";
    m_lockWindow->hideLockWindow();
}

uint KSldApp::activeTime() const
{
    sonicDebug() << "Active time requested";

    if (m_lockedTimer.isValid()) {
        return m_lockedTimer.elapsed();
    }
    return 0;
}

bool KSldApp::isGraceTime() const
{
    return m_inGraceTime;
}

void KSldApp::endGraceTime()
{
    sonicDebug() << "Ending grace time";
    m_graceTimer->stop();
    m_inGraceTime = false;
}

void KSldApp::unlock()
{
    sonicDebug() << "Unlock requested";

    if (isGraceTime() || !m_requirePassword) {
        // the process might take some time to exit; don't wait for that to happen
        doUnlock();
        s_graceTimeKill = true;
        m_lockProcess->terminate();
    }
}

void KSldApp::inhibit()
{
    sonicDebug() << "Inhibit requested";
    ++m_inhibitCounter;
    updateIdleTimeout();
}

void KSldApp::uninhibit()
{
    sonicDebug() << "Uninhibit requested";
    --m_inhibitCounter;
    updateIdleTimeout();
}

void KSldApp::updateIdleTimeout()
{
    const bool hasTimeout = m_idleId;
    const bool timeoutConfigured = KScreenSaverSettings::autolock() && KScreenSaverSettings::timeout() > 0;
    const bool inhibited = isInhibited();
    if (hasTimeout && inhibited) {
        KIdleTime::instance()->removeIdleTimeout(m_idleId);
        m_idleId = 0;
    } else if (!hasTimeout && !inhibited && timeoutConfigured) {
        std::chrono::duration<double, std::chrono::minutes::period> timeout(KScreenSaverSettings::timeout());
        if (QGuiApplication::platformName() == QLatin1StringView("xcb")) {
            timeout += std::chrono::milliseconds(KIdleTime::instance()->idleTime());
        }
        m_idleId = KIdleTime::instance()->addIdleTimeout(std::chrono::duration_cast<std::chrono::milliseconds>(timeout));
    }
}

void KSldApp::solidSuspend()
{
    sonicDebug() << "Solid suspend called";

    // ignore in case that we use logind
    if (m_logind && m_logind->isConnected()) {
        return;
    }
    if (KScreenSaverSettings::lockOnResume()) {
        sonicDebug() << "Solid suspend called, locking now";
        lock(EstablishLock::Immediate);
    }
}

void KSldApp::lockScreenShown()
{
    sonicDebug() << "lockScreenShown(): m_lockState:" << m_lockState;

    if (m_lockState == Locked) {
        return;
    }
    m_lockState = Locked;
    m_lockedTimer.restart();
    if (m_logind) {
        m_logind->uninhibit();
        m_logind->setLocked(true);
    }
    if (m_lockGrace > 0 && m_inGraceTime) {
        m_graceTimer->start(m_lockGrace);
    }
    if (m_globalAccel) {
        m_globalAccel->prepare();
    }
    Q_EMIT locked();
    Q_EMIT lockStateChanged();
}

void KSldApp::setGreeterEnvironment(const QProcessEnvironment &env)
{
    sonicDebug() << "Setting greeter environment";

    m_greeterEnv = env;
}

bool KSldApp::event(QEvent *event)
{
    sonicDebug() << "Event received";

    if (event->type() == QEvent::KeyPress && m_globalAccel) {
        if (m_globalAccel->keyEvent(static_cast<QKeyEvent *>(event))) {
            event->setAccepted(true);
        }
    }
    return false;
}

} // namespace

#include "moc_ksldapp.cpp"
