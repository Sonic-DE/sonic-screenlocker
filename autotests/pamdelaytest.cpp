/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QDBusMessage>
#include <QDBusPendingReply>
#include <QDBusServer>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>
#include <optional>

#include "../greeter/pamauthenticator.h"
#include "../greeter/worker/result.h"

class WorkerPeer : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.plasma.screenlocker")

public Q_SLOTS:
    void Ping(const QString &)
    {
        Q_EMIT pinged();
    }
    void StartFailedDelay(uint)
    {
    }

Q_SIGNALS:
    void pinged();
};

class PamDelayTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void propertyContract();
    void workerInitializationProtocol();
    void earlyRequestWaitsForWorkerReady();
    void cancelQueuedRequestBeforeReady();
    void readyPropertyTracksWorkerLifecycle();
    void workerFailureClearsReady();
    void cancelDuringReadyNotificationCanRestart();
    void immediateRestartIsDeferred();
    void overlappingDelaysKeepLongestDeadline();
    void cancelAndRestartIgnoresStaleResponses();
    void cancelDuringBusyNotificationCanRestart();
    void cancelDuringPromptNotificationCanRestart();
    void workerSlotsRejectDirectCalls();
    void repeatedWrongPasswordsRemainAvailable();
    void promptlessFailureLoopBecomesUnavailable();
};

void PamDelayTest::initTestCase()
{
    QVERIFY(qgetenv("LD_PRELOAD").contains("pam_wrapper"));
    QVERIFY(QFileInfo::exists(QStringLiteral(PAM_TEST_SERVICE_DIR) + QStringLiteral("/failure_delay_test")));
}

void PamDelayTest::propertyContract()
{
    const QMetaObject &metaObject = PamAuthenticator::staticMetaObject;
    const int propertyIndex = metaObject.indexOfProperty("inPasswordDelay");
    QVERIFY(propertyIndex >= 0);
    const QMetaProperty property = metaObject.property(propertyIndex);
    QCOMPARE(property.metaType(), QMetaType::fromType<bool>());
    QVERIFY(property.isReadable());
    QVERIFY(!property.isWritable());
    QVERIFY(property.hasNotifySignal());

    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QVERIFY(!authenticator.inPasswordDelay());
}

void PamDelayTest::immediateRestartIsDeferred()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy failedSpy(&authenticator, &PamAuthenticator::failed);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);
    QSignalSpy delayStartedSpy(&authenticator, &PamAuthenticator::loginFailedDelayStarted);
    QSignalSpy delayChangedSpy(&authenticator, &PamAuthenticator::inPasswordDelayChanged);
    QVERIFY(promptSpy.isValid());
    QVERIFY(failedSpy.isValid());
    QVERIFY(succeededSpy.isValid());
    QVERIFY(delayStartedSpy.isValid());
    QVERIFY(delayChangedSpy.isValid());

    connect(&authenticator, &PamAuthenticator::failed, &authenticator, &PamAuthenticator::tryUnlock);
    authenticator.tryUnlock();
    QVERIFY(promptSpy.wait());
    authenticator.respond(QByteArrayLiteral("wrong"));

    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(delayStartedSpy.count(), 1, 1000);
    QVERIFY(authenticator.inPasswordDelay());
    QTest::qWait(20);
    QCOMPARE(promptSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 1);

    QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), 2, 2000);
    QVERIFY(!authenticator.inPasswordDelay());
    QCOMPARE(delayChangedSpy.count(), 2);
    authenticator.respond(QByteArrayLiteral("my_password"));
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 1000);
}

void PamDelayTest::readyPropertyTracksWorkerLifecycle()
{
    const auto &metaObject = PamAuthenticator::staticMetaObject;
    const int propertyIndex = metaObject.indexOfProperty("ready");
    QVERIFY(propertyIndex >= 0);
    const auto property = metaObject.property(propertyIndex);
    QCOMPARE(property.metaType(), QMetaType::fromType<bool>());
    QVERIFY(property.isReadable());
    QVERIFY(!property.isWritable());
    QVERIFY(property.hasNotifySignal());

    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy readySpy(&authenticator, &PamAuthenticator::readyChanged);
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);
    QVERIFY(!authenticator.isReady());
    QTRY_VERIFY_WITH_TIMEOUT(authenticator.isReady(), 2000);
    QCOMPARE(readySpy.count(), 1);
    authenticator.cancel();
    QVERIFY(!authenticator.isReady());
    QCOMPARE(readySpy.count(), 2);
    authenticator.tryUnlock();
    QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), 1, 2000);
    QVERIFY(authenticator.isReady());
    QCOMPARE(readySpy.count(), 3);
    authenticator.respond(QByteArrayLiteral("my_password"));
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 1000);
}

void PamDelayTest::earlyRequestWaitsForWorkerReady()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy readySpy(&authenticator, &PamAuthenticator::readyChanged);
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);
    bool promptBeforeReady = false;
    connect(&authenticator, &PamAuthenticator::promptForSecret, &authenticator, [&] {
        promptBeforeReady |= !authenticator.isReady();
    });

    QVERIFY(!authenticator.isReady());
    authenticator.tryUnlock();
    const auto requestId = authenticator.currentRequestId();
    QVERIFY(requestId > 0);
    QVERIFY(!authenticator.isReady());
    QVERIFY(!authenticator.isBusy());
    QCOMPARE(promptSpy.count(), 0);
    authenticator.tryUnlock();
    QCOMPARE(authenticator.currentRequestId(), requestId);
    QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), 1, 2000);
    QCOMPARE(readySpy.count(), 1);
    QVERIFY(!promptBeforeReady);
    authenticator.respond(QByteArrayLiteral("my_password"));
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 1000);
}

void PamDelayTest::workerInitializationProtocol()
{
    WorkerPeer peer;
    QSignalSpy pingSpy(&peer, &WorkerPeer::pinged);
    std::optional<QDBusConnection> connection;
    QDBusServer server;
    QVERIFY(server.isConnected());
    bool registered = false;
    connect(&server, &QDBusServer::newConnection, &peer, [&](const QDBusConnection &incoming) {
        auto c = incoming;
        registered = c.registerObject(QStringLiteral("/org/kde/plasma/screenlocker"), &peer, QDBusConnection::ExportAllSlots);
        connection = c;
    });

    QProcess worker;
    worker.setProgram(QCoreApplication::applicationDirPath() + QStringLiteral("/kscreenlocker_worker"));
    worker.setArguments({QStringLiteral("promptless_failure_test")});
    worker.start();
    worker.write(server.address().toUtf8());
    worker.closeWriteChannel();
    QTRY_VERIFY_WITH_TIMEOUT(connection.has_value(), 2000);
    QVERIFY(registered);
    QTRY_COMPARE_WITH_TIMEOUT(pingSpy.count(), 1, 2000);

    const auto authenticate = QDBusMessage::createMethodCall(QString(),
                                                             QStringLiteral("/org/kde/plasma/screenlocker/worker"),
                                                             QStringLiteral("org.kde.plasma.screenlocker.worker"),
                                                             QStringLiteral("Authenticate"));
    QDBusPendingReply<int> beforeStart = connection->asyncCall(authenticate);
    QTRY_VERIFY_WITH_TIMEOUT(beforeStart.isFinished(), 2000);
    QVERIFY(!beforeStart.isError());
    QCOMPARE(beforeStart.value(), static_cast<int>(WorkerResult::Unavailable));

    auto start = QDBusMessage::createMethodCall(QString(),
                                                QStringLiteral("/org/kde/plasma/screenlocker/worker"),
                                                QStringLiteral("org.kde.plasma.screenlocker.worker"),
                                                QStringLiteral("Start"));
    // The service comes from argv; Start accepts only the username and must acknowledge initialization.
    start.setArguments({QStringLiteral("test_user")});
    QDBusPendingReply<> started = connection->asyncCall(start);
    QTRY_VERIFY_WITH_TIMEOUT(started.isFinished(), 2000);
    QVERIFY(!started.isError());
    QDBusPendingReply<int> afterStart = connection->asyncCall(authenticate);
    QTRY_VERIFY_WITH_TIMEOUT(afterStart.isFinished(), 2000);
    QVERIFY(!afterStart.isError());
    // The argv-selected fixture fails without a prompt; it is a valid initialized PAM stack.
    QCOMPARE(afterStart.value(), static_cast<int>(WorkerResult::Failure));

    worker.kill();
    QVERIFY(worker.waitForFinished(2000));
    QDBusConnection::disconnectFromPeer(connection->name());
}

void PamDelayTest::cancelQueuedRequestBeforeReady()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy readySpy(&authenticator, &PamAuthenticator::readyChanged);
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);
    QVERIFY(!authenticator.isReady());
    authenticator.tryUnlock();
    const auto cancelledRequest = authenticator.currentRequestId();
    authenticator.cancel();
    QTest::qWait(50);
    QVERIFY(!authenticator.isReady());
    QCOMPARE(readySpy.count(), 0);
    QCOMPARE(promptSpy.count(), 0);
    QCOMPARE(succeededSpy.count(), 0);
    authenticator.tryUnlock();
    QVERIFY(authenticator.currentRequestId() > cancelledRequest);
    QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), 1, 2000);
    QVERIFY(authenticator.isReady());
    authenticator.respond(QByteArrayLiteral("my_password"));
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 1000);
}

void PamDelayTest::cancelDuringReadyNotificationCanRestart()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy cancelledSpy(&authenticator, &PamAuthenticator::authenticationCancelled);
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);
    const auto connection = connect(&authenticator, &PamAuthenticator::readyChanged, &authenticator, [&] {
        if (authenticator.isReady()) {
            authenticator.cancel();
        }
    });
    QTRY_COMPARE_WITH_TIMEOUT(cancelledSpy.count(), 1, 2000);
    QVERIFY(!authenticator.isReady());
    QCOMPARE(promptSpy.count(), 0);
    disconnect(connection);
    authenticator.tryUnlock();
    QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), 1, 2000);
    authenticator.respond(QByteArrayLiteral("my_password"));
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 1000);
}

void PamDelayTest::workerFailureClearsReady()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy readySpy(&authenticator, &PamAuthenticator::readyChanged);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);
    QTRY_VERIFY_WITH_TIMEOUT(authenticator.isReady(), 2000);
    auto worker = authenticator.findChild<QProcess *>();
    QVERIFY(worker);
    worker->kill();
    QTRY_VERIFY_WITH_TIMEOUT(authenticator.isUnavailable(), 2000);
    QVERIFY(!authenticator.isReady());
    QCOMPARE(readySpy.count(), 2);
    QCOMPARE(succeededSpy.count(), 0);
    const auto requestId = authenticator.currentRequestId();
    authenticator.tryUnlock();
    QCOMPARE(authenticator.currentRequestId(), requestId);
}

void PamDelayTest::overlappingDelaysKeepLongestDeadline()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy delayChangedSpy(&authenticator, &PamAuthenticator::inPasswordDelayChanged);
    authenticator.StartFailedDelay(200000);
    QCOMPARE(delayChangedSpy.count(), 1);
    QVERIFY(authenticator.inPasswordDelay());
    authenticator.StartFailedDelay(10000);
    QTest::qWait(50);
    QCOMPARE(delayChangedSpy.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(delayChangedSpy.count(), 2, 1000);
    QVERIFY(!authenticator.inPasswordDelay());
}

void PamDelayTest::cancelAndRestartIgnoresStaleResponses()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);
    authenticator.tryUnlock();
    QVERIFY(promptSpy.wait());
    const auto oldRequest = authenticator.currentRequestId();
    authenticator.tryUnlock();
    QCOMPARE(authenticator.currentRequestId(), oldRequest);
    QVERIFY(authenticator.isPromptReady());
    authenticator.cancel();
    QVERIFY(!authenticator.isPromptReady());
    authenticator.respond(QByteArrayLiteral("my_password"));
    authenticator.tryUnlock();
    QVERIFY(authenticator.currentRequestId() > oldRequest);
    QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), 2, 2000);
    QCOMPARE(succeededSpy.count(), 0);
    authenticator.respond(QByteArrayLiteral("my_password"));
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 1000);
}

void PamDelayTest::repeatedWrongPasswordsRemainAvailable()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy failedSpy(&authenticator, &PamAuthenticator::failed);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);

    for (int attempt = 1; attempt <= 5; ++attempt) {
        authenticator.tryUnlock();
        QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), attempt, 2000);
        authenticator.respond(QByteArrayLiteral("wrong"));
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), attempt, 1000);
        QVERIFY(authenticator.isAvailable());
        QVERIFY(!authenticator.isUnavailable());
    }

    authenticator.tryUnlock();
    QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), 6, 2000);
    authenticator.respond(QByteArrayLiteral("my_password"));
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 1000);
}

void PamDelayTest::cancelDuringBusyNotificationCanRestart()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy cancelledSpy(&authenticator, &PamAuthenticator::authenticationCancelled);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);
    const auto connection = connect(&authenticator, &PamAuthenticator::busyChanged, &authenticator, [&] {
        if (authenticator.isBusy()) {
            authenticator.cancel();
        }
    });

    authenticator.tryUnlock();
    QTRY_COMPARE_WITH_TIMEOUT(cancelledSpy.count(), 1, 2000);
    QCOMPARE(promptSpy.count(), 0);
    QCOMPARE(succeededSpy.count(), 0);
    QVERIFY(!authenticator.isUnavailable());
    disconnect(connection);
    authenticator.tryUnlock();
    QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), 1, 2000);
    authenticator.respond(QByteArrayLiteral("my_password"));
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 1000);
}

void PamDelayTest::cancelDuringPromptNotificationCanRestart()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy cancelledSpy(&authenticator, &PamAuthenticator::authenticationCancelled);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);
    const auto connection = connect(&authenticator, &PamAuthenticator::promptReadyChanged, &authenticator, [&] {
        if (authenticator.isPromptReady()) {
            authenticator.cancel();
        }
    });

    authenticator.tryUnlock();
    QTRY_COMPARE_WITH_TIMEOUT(cancelledSpy.count(), 1, 2000);
    QCOMPARE(promptSpy.count(), 0);
    QCOMPARE(succeededSpy.count(), 0);
    QVERIFY(!authenticator.isPromptReady());
    disconnect(connection);
    authenticator.tryUnlock();
    QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), 1, 2000);
    authenticator.respond(QByteArrayLiteral("my_password"));
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 1000);
}

void PamDelayTest::promptlessFailureLoopBecomesUnavailable()
{
    PamAuthenticator authenticator(QStringLiteral("promptless_failure_test"), QStringLiteral("test_user"));
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::prompt);
    QSignalSpy secretPromptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy failedSpy(&authenticator, &PamAuthenticator::failed);
    QSignalSpy availableSpy(&authenticator, &PamAuthenticator::availableChanged);
    connect(&authenticator, &PamAuthenticator::failed, &authenticator, &PamAuthenticator::tryUnlock);

    authenticator.tryUnlock();
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 4, 2000);
    QVERIFY(authenticator.isUnavailable());
    QVERIFY(!authenticator.isUnlocked());
    QCOMPARE(availableSpy.count(), 1);
    QCOMPARE(promptSpy.count(), 0);
    QCOMPARE(secretPromptSpy.count(), 0);
    const auto lastRequest = authenticator.currentRequestId();
    authenticator.tryUnlock();
    QCOMPARE(authenticator.currentRequestId(), lastRequest);
    QTest::qWait(50);
    QCOMPARE(failedSpy.count(), 4);
}

void PamDelayTest::workerSlotsRejectDirectCalls()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy succeededSpy(&authenticator, &PamAuthenticator::succeeded);
    authenticator.tryUnlock();
    QTRY_COMPARE_WITH_TIMEOUT(promptSpy.count(), 1, 2000);
    authenticator.Ping(QStringLiteral("not a D-Bus call"));
    QCOMPARE(authenticator.Prompt(QStringLiteral("fake prompt")), QString());
    QCOMPARE(authenticator.MaskedPrompt(QStringLiteral("fake prompt")), QString());
    QCOMPARE(promptSpy.count(), 1);
    QVERIFY(authenticator.isPromptReady());
    authenticator.respond(QByteArrayLiteral("my_password"));
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 1000);
}

QTEST_GUILESS_MAIN(PamDelayTest)

#include "pamdelaytest.moc"
