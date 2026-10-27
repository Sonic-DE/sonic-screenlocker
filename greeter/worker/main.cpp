// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2020 David Edmundson <davidedmundson@kde.org>
// SPDX-FileCopyrightText: 2026 Harald Sitter <sitter@kde.org>

#include <security/pam_appl.h>
#include <unistd.h>

#include <iostream>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>

#include "config-kscreenlocker.h"
#include "config-worker.h"
#include "org.kde.plasma.screenlocker.h"
#include "prctls.h"
#include "result.h"

#include <Sonic/Logging/Logging>

SONIC_LOGGING_COMPONENT("org.sonicde.screenlocker.worker")

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace std {
template<>
struct default_delete<pam_handle_t> {
    void operator()(pam_handle_t *ptr) const
    {
        if (ptr) {
            ::pam_end(ptr, PAM_SUCCESS);
        }
    }
};
} // namespace std

namespace {

template<typename Output, typename Input>
[[nodiscard]] Output narrow(Input i)
{
    Output o = i;
    if (i != Input(o)) {
        std::abort();
    }
    if (const auto sameSignedness = (std::is_signed_v<Input> && std::is_signed_v<Output>); !sameSignedness && ((i < Input{}) != (o < Output{}))) {
        std::abort();
    }
    return o;
}

[[nodiscard]] std::string readAddressFromStdin()
{
    std::string address;
    if (!std::getline(std::cin, address)) {
        sonicWarning() << "Failed to read worker D-Bus address from stdin.";
        return {};
    }
    return address;
}

class Worker : public QObject {
    Q_OBJECT
public:
    Worker(const QString &service, const QString &user, OrgKdePlasmaScreenlockerInterface *screenlocker);
    [[nodiscard]] WorkerResult::Type authenticate();
    void startFailedDelay(uint useconds);

Q_SIGNALS:
    void inAuthenticateChanged(bool inAuthenticate);

    // internal
    void promptResponseReceived(const QByteArray &prompt);
    void cancelled();
    void interrupt();

private:
    [[nodiscard]] static int converse(int n, const struct pam_message **msg, struct pam_response **resp, void *data);

    QString m_service;
    QString m_user;
    bool m_fingerprint;
    struct pam_conv m_conv;
    bool m_available = true;
    bool m_inAuthenticate = false;
    std::chrono::steady_clock::time_point m_nextAttemptAllowedTime;
    int m_result = -1;
    OrgKdePlasmaScreenlockerInterface *m_screenlocker;

    // Initialized based on other members, keep last!
    std::unique_ptr<pam_handle_t> m_handle = nullptr; //< the actual PAM handle
};

class Adaptor : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.plasma.screenlocker.worker")
public:
    Adaptor(const QString &service, const QString &user, OrgKdePlasmaScreenlockerInterface &screenlocker)
        : QObject(nullptr)
        , m_screenlocker(screenlocker)
        , m_worker(service, user, &m_screenlocker)
    {
    }

public Q_SLOTS:
    [[nodiscard]] int Authenticate()
    {
        sonicDebug() << "Proxy: Authenticate called";
        return m_worker.authenticate();
    }

    Q_NOREPLY void Cancel()
    {
        sonicDebug() << "Proxy: Cancel called";
        qApp->quit();
    }

private:
    OrgKdePlasmaScreenlockerInterface &m_screenlocker;
    Worker m_worker;
};

void fail_delay(int retval, unsigned usec_delay, void *appdata_ptr)
{
    auto *worker = reinterpret_cast<Worker *>(appdata_ptr); // Refer the pam_conv (@sa m_conv) structure for info on appdata_ptr
    if (!worker) {
        sonicFatal() << "[PAM worker] appdata_ptr not convertible to a valid Worker! Cannot apply fail delay";
        return;
    }
    if (retval == PAM_SUCCESS) {
        sonicDebug() << "[PAM worker] Fail delay function was called, but authentication result was a success!";
        return;
    }
    worker->startFailedDelay(usec_delay);
}

} // namespace

int Worker::converse(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    auto c = static_cast<Worker *>(data);

    if (!resp) {
        sonicWarning() << "[PAM worker] Converse called with null resp pointer";
        return PAM_BUF_ERR;
    }

    const auto nSize = narrow<size_t>(n);

    *resp = static_cast<struct pam_response *>(calloc(n, sizeof(struct pam_response)));
    if (!*resp) {
        return PAM_BUF_ERR;
    }
    auto responses = std::span{*resp, nSize};

    auto messages = std::span{msg, nSize};
    Q_ASSERT_X(responses.size() == messages.size(), Q_FUNC_INFO, "Number of PAM messages and responses should be the same");

    for (const auto &[pamMessage, pamResponse] : std::views::zip(messages, responses)) {
        bool isSecret = false;
        switch (pamMessage->msg_style) {
        case PAM_PROMPT_ECHO_OFF: {
            isSecret = true;
            Q_FALLTHROUGH();
        case PAM_PROMPT_ECHO_ON:
            const QString prompt = QString::fromLocal8Bit(pamMessage->msg);

            sonicDebug().noquote() << "[PAM worker" << c->m_service + u"] Message:"_s << (isSecret ? "Echo-off prompt:" : "Echo-on prompt:") << prompt;

            const QString responseString = [&] {
                if (isSecret) {
                    QDBusPendingReply<QString> reply = c->m_screenlocker->MaskedPrompt(prompt);
                    reply.waitForFinished();
                    if (reply.error().type() == QDBusError::Disconnected) {
                        _exit(0);
                    }
                    return reply.value();
                }
                return c->m_screenlocker->Prompt(prompt).value();
            }();
            const auto response = responseString.toUtf8();

            const auto responseLengthIncludingNull = response.length() + 1; // QByteArray holds an implicit \0 at the end.
            pamResponse.resp = static_cast<char *>(malloc(responseLengthIncludingNull));
            std::copy_n(response.constData(), responseLengthIncludingNull, pamResponse.resp);

            break;
        }
        case PAM_ERROR_MSG: {
            const QString error = QString::fromLocal8Bit(pamMessage->msg);
            sonicDebug().noquote() << "[PAM worker" << c->m_service + u"] Message: Error message:"_s << error;
            c->m_screenlocker->ErrorMessage(error);
            break;
        }
        case PAM_TEXT_INFO: {
            // if there's only the info message, let's predict the prompts too
            const QString info = QString::fromLocal8Bit(pamMessage->msg);
            sonicDebug().noquote() << "[PAM worker" << c->m_service + u"] Message: Info message:"_s << info;
            c->m_screenlocker->InfoMessage(info);
            break;
        }
        default:
            sonicDebug().noquote() << "[PAM worker" << c->m_service + u"] Message: Unhandled message type:"_s << pamMessage->msg_style;
            break;
        }
    }

    return PAM_SUCCESS;
}

Worker::Worker(const QString &service, const QString &user, OrgKdePlasmaScreenlockerInterface *screenlocker)
    : QObject(nullptr)
    , m_service(service)
    , m_user(user)
    , m_fingerprint(service == KSCREENLOCKER_PAM_FINGERPRINT_SERVICE)
    , m_conv({.conv = &Worker::converse, .appdata_ptr = this})
    , m_screenlocker(screenlocker)
    , m_handle([service, user, this]() -> std::unique_ptr<pam_handle_t> {
        std::unique_ptr<pam_handle_t> handle;
        m_result = pam_start(qPrintable(service), user.isEmpty() ? nullptr : qPrintable(user), &m_conv, std::out_ptr(handle));

        if (m_result != PAM_SUCCESS) {
            m_available = false;
            sonicWarning().noquote() << "[PAM worker" << service + u"] start: error starting, result code:"_s << m_result
                                     << u"(%1)"_s.arg(QString::fromLocal8Bit(pam_strerror(handle.get(), m_result)));
            return nullptr;
        }

#if KSCREENLOCKER_HAVE_PAM_FAIL_DELAY
        const int delayResult = pam_set_item(handle.get(), PAM_FAIL_DELAY, reinterpret_cast<void *>(fail_delay));
        if (delayResult != PAM_SUCCESS) {
            sonicWarning() << "Could not install PAM failure-delay callback" << delayResult;
        }
#else
        Q_UNUSED(fail_delay);
#endif

        sonicDebug().noquote() << "[PAM worker" << service + u"] start: successfully started"_s;

        return handle;
    }())
{
}

WorkerResult::Type Worker::authenticate()
{
    if (m_inAuthenticate) {
        sonicDebug().noquote() << "[PAM worker" << m_service + u"] Authentication is already in progress"_s;
        return WorkerResult::Type::Failure;
    }
    if (!m_available || !m_handle || m_result != PAM_SUCCESS) {
        sonicDebug().noquote() << "[PAM worker" << m_service + u"] PAM service is not available"_s;
        return WorkerResult::Type::Unavailable;
    }

    if (const auto remaining = m_nextAttemptAllowedTime - std::chrono::steady_clock::now(); remaining > std::chrono::steady_clock::duration::zero()) {
        QThread::sleep(std::chrono::ceil<std::chrono::nanoseconds>(remaining));
    }

    m_inAuthenticate = true;
    Q_EMIT inAuthenticateChanged(m_inAuthenticate);
    auto scopedAuthenticate = qScopeGuard([this] {
        m_inAuthenticate = false;
        Q_EMIT inAuthenticateChanged(m_inAuthenticate);
    });

    sonicDebug().noquote() << "[PAM worker" << m_service + u"] Authenticate: Starting authentication"_s;

    QElapsedTimer timer;
    timer.start();

    int rc = pam_authenticate(m_handle.get(), 0); // PAM_SILENT);
    sonicDebug().noquote() << "[PAM worker" << m_service + u"] Authenticate: Authentication done, result code:"_s << rc
                           << u"(%1)"_s.arg(QString::fromLocal8Bit(pam_strerror(m_handle.get(), rc)));

    sonicWarning() << timer.elapsed() << "ms elapsed during pam_authenticate call for service" << qUtf8Printable(m_service) << "with result code" << rc;

    if (rc == PAM_SUCCESS) {
        pam_setcred(m_handle.get(), PAM_REFRESH_CRED);
        /* ignore errors on refresh credentials. If this did not work we use the old ones. */
        return WorkerResult::Type::Success;
    }

    if (rc == PAM_AUTHINFO_UNAVAIL && m_fingerprint) {
        // For fingerprint authentication, PAM_AUTHINFO_UNAVAIL can mean any number of things, but luckily most of them
        // are fixed by simply restarting the authentication. The notable case we want to catch here is timeouts.
        constexpr auto tooQuick = 250ms;
        // Unless this error was returned suspiciously fast. Then it probably was an actual problem.
        if (timer.durationElapsed() <= tooQuick) {
            sonicWarning() << "Unexpectedly short auth error on fingerprint reader" << timer.durationElapsed();
            m_available = false;
            return WorkerResult::Type::Unavailable;
        }
        return WorkerResult::Type::Failure;
    }

    if (rc == PAM_AUTHINFO_UNAVAIL || rc == PAM_MODULE_UNKNOWN) {
        // Explicitly unavailable -> let's mark as such
        m_available = false;
        return WorkerResult::Type::Unavailable;
    }

    return WorkerResult::Type::Failure;
}

void Worker::startFailedDelay(uint useconds)
{
    m_nextAttemptAllowedTime = std::max(m_nextAttemptAllowedTime, std::chrono::steady_clock::now() + std::chrono::microseconds(useconds));
    // Report failure promptly; enforce the deadline on the next attempt, not by blocking the failure notification.
    m_screenlocker->StartFailedDelay(useconds);
}

int main(int argc, char *argv[])
{
    const auto parentPid = getppid();
    if (parentPid == 1) {
        sonicWarning() << "Worker has no greeter parent, exiting.";
        return 1;
    }
    const auto parentDeathSignal = PRCTLs::dieWithParent();
    if (!parentDeathSignal) {
        sonicWarning() << "Failed to set parent-death signal, errno:" << parentDeathSignal.error();
        return 1;
    }
    if (getppid() != parentPid) {
        sonicWarning() << "Greeter parent changed during worker startup, exiting.";
        return 1;
    }

    // Moving PAM into a subprocess must not undo the greeter's password/ptrace protection.
    const auto dumpProtection = PRCTLs::setDumpable(false);
    if (!dumpProtection) {
        sonicWarning() << "Failed to disable worker dump/ptrace access, errno:" << dumpProtection.error();
        return 1;
    }
    if (auto dumpable = sonicLoggingComponent().isDebugEnabled(); !PRCTLs::setDumpable(dumpable)) {
        sonicWarning() << "Failed to set dumpable flag on the worker" << (dumpable ? "enabled" : "disabled");
        // We'll continue but it is a bit unexpected.
    }

    QCoreApplication app(argc, argv);

    sonicDebug() << "Worker is starting up.";

    constexpr auto expectedArguments = 3; // [binary, pam-service-name, username]
    if (app.arguments().size() < expectedArguments) {
        sonicWarning() << "Worker requires service and username arguments. Also, don't call this manually.";
        return 1;
    }
    auto service = app.arguments().at(1); // the PAM service name (e.g. kde-fingerprint)
    auto user = app.arguments().at(2);

    // One log per PAM service, so concurrent workers (password, fingerprint, ...) stay apart.
    Sonic::Logging::install({.binaryName = u"kscreenlocker_worker-%1"_s.arg(service)});

    auto address = readAddressFromStdin();
    if (address.empty()) {
        sonicWarning() << "Failed to read D-Bus address from stdin, exiting.";
        return 1;
    }

    auto connection = QDBusConnection::connectToPeer(QString::fromStdString(address), u"org.kde.plasma.screenlocker"_s);
    OrgKdePlasmaScreenlockerInterface screenlocker(QString(), u"/org/kde/plasma/screenlocker"_s, connection);
    screenlocker.setTimeout(
        std::numeric_limits<int>::max()); // disable timeout, we expect blocking calls to arrive eventually (or we get terminated by our parent)
    Adaptor proxy(service, user, screenlocker);
    connection.registerObject(u"/org/kde/plasma/screenlocker/worker"_s, &proxy, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
    // Tell the screenlocker we are ready. This is necessary because there is technically a race between
    // the connection getting established and us registering the object. To avoid any issues we have this
    // explicit "go" call.
    screenlocker.Ping(u"Hello from worker!"_s);

    sonicDebug() << "Worker is now running, waiting for D-Bus calls.";

    auto ret = app.exec();
    sonicDebug() << "Worker is exiting with code" << ret;
    return ret;
}

#include "main.moc"
