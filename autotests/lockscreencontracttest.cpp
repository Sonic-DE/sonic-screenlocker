/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QFile>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QTest>

#include "../greeter/pamauthenticators.h"

class LockScreenContractTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void backendContract();
    void productionThemeUsesBackendContract();
};

void LockScreenContractTest::backendContract()
{
    const QMetaObject &metaObject = PamAuthenticators::staticMetaObject;
    const int delayPropertyIndex = metaObject.indexOfProperty("inPasswordDelay");
    const int pendingPropertyIndex = metaObject.indexOfProperty("hasPendingResponse");
    const int suspendedPropertyIndex = metaObject.indexOfProperty("suspended");
    QVERIFY(delayPropertyIndex >= 0);
    QVERIFY(pendingPropertyIndex >= 0);
    QVERIFY(suspendedPropertyIndex >= 0);
    QVERIFY(metaObject.property(delayPropertyIndex).hasNotifySignal());
    QVERIFY(metaObject.property(pendingPropertyIndex).hasNotifySignal());
    QVERIFY(metaObject.property(suspendedPropertyIndex).hasNotifySignal());
    QVERIFY(metaObject.indexOfMethod("submitResponse(QString,QObject*)") >= 0);
    QVERIFY(metaObject.indexOfMethod("discardPendingResponse()") >= 0);
    QVERIFY(metaObject.indexOfMethod("setSuspended(bool)") >= 0);
}

void LockScreenContractTest::productionThemeUsesBackendContract()
{
    QFile lockScreenUi(QStringLiteral(SCREENLOCKER_LOCKSCREEN_UI));
    QFile mainBlock(QStringLiteral(SCREENLOCKER_MAIN_BLOCK));
    QVERIFY2(lockScreenUi.open(QIODevice::ReadOnly), qPrintable(lockScreenUi.errorString()));
    QVERIFY2(mainBlock.open(QIODevice::ReadOnly), qPrintable(mainBlock.errorString()));
    const QByteArray ui = lockScreenUi.readAll();
    const QByteArray block = mainBlock.readAll();

    QVERIFY(!ui.contains("pamTimeout"));
    QVERIFY(!ui.contains("pendingPassword"));
    QVERIFY(ui.contains("authenticator.submitResponse(password, lockScreenUi)"));
    QVERIFY(ui.contains("authenticator.setSuspended(true)"));
    QVERIFY(ui.contains("authenticator.setSuspended(false)"));
    QVERIFY(block.contains("property bool submissionBlocked"));
    QVERIFY(block.contains("if (submissionBlocked)"));
}

QTEST_GUILESS_MAIN(LockScreenContractTest)

#include "lockscreencontracttest.moc"
