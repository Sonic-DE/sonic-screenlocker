/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QFileInfo>
#include <QSignalSpy>
#include <QTest>

#include "../greeter/pamauthenticator.h"

class PamDelayTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void propertyContract();
    void immediateRestartIsDeferred();
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

QTEST_GUILESS_MAIN(PamDelayTest)

#include "pamdelaytest.moc"
