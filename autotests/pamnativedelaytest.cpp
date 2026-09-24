/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include "../greeter/pamauthenticator.h"

class PamNativeDelayTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void libraryManagedDelayKeepsFrontendResponsive();
};

void PamNativeDelayTest::libraryManagedDelayKeepsFrontendResponsive()
{
    PamAuthenticator authenticator(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QSignalSpy promptSpy(&authenticator, &PamAuthenticator::promptForSecret);
    QSignalSpy failedSpy(&authenticator, &PamAuthenticator::failed);
    QSignalSpy delaySpy(&authenticator, &PamAuthenticator::loginFailedDelayStarted);
    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    connect(&heartbeat, &QTimer::timeout, this, [&heartbeats] {
        ++heartbeats;
    });
    heartbeat.start();

    authenticator.tryUnlock();
    QVERIFY(promptSpy.wait());
    authenticator.respond(QByteArrayLiteral("wrong"));
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 2000);
    QVERIFY(heartbeats > 0);
    QCOMPARE(delaySpy.count(), 0);
    QVERIFY(!authenticator.inPasswordDelay());
}

QTEST_GUILESS_MAIN(PamNativeDelayTest)

#include "pamnativedelaytest.moc"
