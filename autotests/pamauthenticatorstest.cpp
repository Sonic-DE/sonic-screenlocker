/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QSignalSpy>
#include <QTest>

#include "../greeter/pamauthenticators.h"

class PamAuthenticatorsTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<PamAuthenticators> createAuthenticators()
    {
        auto interactive = std::make_unique<PamAuthenticator>(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
        return std::make_unique<PamAuthenticators>(std::move(interactive), std::vector<std::unique_ptr<PamAuthenticator>>{});
    }

private Q_SLOTS:
    void queuedResponseWaitsForDelayAndPrompt();
    void ownerDestructionAndSuspendDiscardResponse();
};

void PamAuthenticatorsTest::queuedResponseWaitsForDelayAndPrompt()
{
    auto authenticators = createAuthenticators();
    QObject owner;
    QSignalSpy promptSpy(authenticators.get(), &PamAuthenticators::promptForSecretChanged);
    QSignalSpy failedSpy(authenticators.get(), &PamAuthenticators::failed);
    QSignalSpy succeededSpy(authenticators.get(), &PamAuthenticators::succeeded);
    QSignalSpy pendingSpy(authenticators.get(), &PamAuthenticators::hasPendingResponseChanged);
    QVERIFY(promptSpy.isValid());
    QVERIFY(failedSpy.isValid());
    QVERIFY(succeededSpy.isValid());
    QVERIFY(pendingSpy.isValid());

    connect(authenticators.get(), &PamAuthenticators::failed, authenticators.get(), [&] {
        authenticators->startAuthenticating();
        QVERIFY(authenticators->submitResponse(QStringLiteral("my_password"), &owner));
    });

    authenticators->startAuthenticating();
    QVERIFY(promptSpy.wait());
    QVERIFY(authenticators->submitResponse(QStringLiteral("wrong"), &owner));
    QVERIFY(authenticators->hasPendingResponse());

    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 1000);
    QVERIFY(authenticators->hasPendingResponse());
    const int promptCountAfterFailure = promptSpy.count();
    QTest::qWait(20);
    QCOMPARE(promptSpy.count(), promptCountAfterFailure);
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 2000);
    QVERIFY(!authenticators->hasPendingResponse());
    QVERIFY(authenticators->isUnlocked());
    QVERIFY(pendingSpy.count() >= 4);
}

void PamAuthenticatorsTest::ownerDestructionAndSuspendDiscardResponse()
{
    auto authenticators = createAuthenticators();
    QSignalSpy promptSpy(authenticators.get(), &PamAuthenticators::promptForSecretChanged);
    QSignalSpy suspendedSpy(authenticators.get(), &PamAuthenticators::suspendedChanged);

    authenticators->startAuthenticating();
    auto owner = std::make_unique<QObject>();
    QVERIFY(authenticators->submitResponse(QStringLiteral("my_password"), owner.get()));
    QVERIFY(authenticators->hasPendingResponse());
    owner.reset();
    QVERIFY(!authenticators->hasPendingResponse());

    authenticators->setSuspended(true);
    QCOMPARE(authenticators->state(), PamAuthenticators::Idle);
    QVERIFY(authenticators->isSuspended());
    QCOMPARE(suspendedSpy.count(), 1);
    authenticators->setSuspended(true);
    QCOMPARE(suspendedSpy.count(), 1);

    authenticators->setSuspended(false);
    QVERIFY(!authenticators->isSuspended());
    QCOMPARE(suspendedSpy.count(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(promptSpy.count() >= 1, 1000);
}

QTEST_GUILESS_MAIN(PamAuthenticatorsTest)

#include "pamauthenticatorstest.moc"
