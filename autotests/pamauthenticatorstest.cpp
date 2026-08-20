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
    void missingAuthenticatorIsSafe();
    void earlySubmissionWaitsForWorkerReady();
    void queuedResponseWaitsForDelayAndPrompt();
    void ownerDestructionAndSuspendDiscardResponse();
};

void PamAuthenticatorsTest::missingAuthenticatorIsSafe()
{
    PamAuthenticators authenticators(std::unique_ptr<PamAuthenticator>{}, std::vector<std::unique_ptr<PamAuthenticator>>{});
    QObject owner;
    QSignalSpy succeededSpy(&authenticators, &PamAuthenticators::succeeded);
    QVERIFY(!authenticators.isBusy());
    QVERIFY(!authenticators.inPasswordDelay());
    QVERIFY(authenticators.prompt().isEmpty());
    QVERIFY(authenticators.promptForSecret().isEmpty());
    QVERIFY(authenticators.infoMessage().isEmpty());
    QVERIFY(authenticators.errorMessage().isEmpty());
    QVERIFY(!authenticators.isUnlocked());
    QVERIFY(!authenticators.hasPendingResponse());
    QCOMPARE(authenticators.authenticatorTypes(), PamAuthenticator::NoninteractiveAuthenticatorTypes(PamAuthenticator::None));
    authenticators.startAuthenticating();
    QCOMPARE(authenticators.state(), PamAuthenticators::Idle);
    QVERIFY(!authenticators.submitResponse(QStringLiteral("my_password"), &owner));
    authenticators.respond(QByteArrayLiteral("my_password"));
    authenticators.discardPendingResponse();
    authenticators.stopAuthenticating();
    authenticators.setSuspended(true);
    authenticators.setSuspended(false);
    authenticators.setGraceLocked(true);
    authenticators.setGraceLocked(false);
    authenticators.cancel();
    QCOMPARE(authenticators.state(), PamAuthenticators::Idle);
    QCOMPARE(succeededSpy.count(), 0);
}

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

void PamAuthenticatorsTest::earlySubmissionWaitsForWorkerReady()
{
    auto interactive = std::make_unique<PamAuthenticator>(QStringLiteral("failure_delay_test"), QStringLiteral("test_user"));
    QVERIFY(!interactive->isReady());
    PamAuthenticators authenticators(std::move(interactive), std::vector<std::unique_ptr<PamAuthenticator>>{});
    QObject owner;
    QSignalSpy succeededSpy(&authenticators, &PamAuthenticators::succeeded);
    authenticators.startAuthenticating();
    QCOMPARE(authenticators.state(), PamAuthenticators::Authenticating);
    QVERIFY(authenticators.submitResponse(QStringLiteral("my_password"), &owner));
    QVERIFY(authenticators.hasPendingResponse());
    QTRY_COMPARE_WITH_TIMEOUT(succeededSpy.count(), 1, 2000);
    QVERIFY(authenticators.isUnlocked());
    QVERIFY(!authenticators.hasPendingResponse());
    QCOMPARE(authenticators.state(), PamAuthenticators::Idle);
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
