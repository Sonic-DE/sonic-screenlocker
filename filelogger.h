/*
SPDX-FileCopyrightText: 2026

SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QDateTime>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMessageLogger>
#include <QStandardPaths>
#include <QTextStream>

#include <sys/stat.h>

static const QString s_logFilePath = QStringLiteral("/var/log/sonic/screenlocker.log");
static const QString s_logFileBackupPath = QStringLiteral("/var/log/sonic/screenlocker.log.last");

static bool s_logRotationDone = false;

inline void rotateLogFile()
{
    if (s_logRotationDone) {
        return; // Only do this once per application run
    }
    s_logRotationDone = true;

    QFile logFile(s_logFilePath);
    if (!logFile.exists()) {
        return; // No log file to rotate
    }

    // Delete old backup file if it exists
    QFile backupFile(s_logFileBackupPath);
    if (backupFile.exists()) {
        backupFile.remove();
    }

    // Rename current log file to backup
    logFile.rename(s_logFileBackupPath);
}

inline bool isJournalctlAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("journalctl")).isEmpty();
}

inline void ensureLogFileExists()
{
    // Only check/create the file, not the directory (directory is created at install time)
    QFile logFile(s_logFilePath);
    if (!logFile.exists()) {
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logFile.close();
        }
        chmod(s_logFilePath.toUtf8().constData(), 0666);
    }
}

inline void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    ensureLogFileExists();
    QFile logFile(s_logFilePath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&logFile);
        const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        QString typeStr;
        switch (type) {
        case QtInfoMsg:
            typeStr = QStringLiteral("(II) ");
            break;
        case QtDebugMsg:
            typeStr = QStringLiteral("(DD) ");
            break;
        case QtWarningMsg:
            typeStr = QStringLiteral("(WW) ");
            break;
        case QtCriticalMsg:
        case QtFatalMsg:
            typeStr = QStringLiteral("(EE) ");
            break;
        default:
            typeStr = QStringLiteral("");
            break;
        }
        const QString category = context.category ? QStringLiteral("%1").arg(context.category) : QStringLiteral("unknown");
        out << timestamp << " " << typeStr << "[" << category << "] " << msg << "\n";
        out.flush();
        logFile.close();
    }
}

[[maybe_unused]] inline void installFileLogger()
{
    // Rotate log file once per application run
    rotateLogFile();

    // Disable Qt internal debug messages at the source
    QLoggingCategory::setFilterRules(
        QStringLiteral("qt.qpa.*.debug=false\n"
                       "qt.qpa.input.*.debug=false\n"
                       "qt.qpa.xcb.*.debug=false\n"
                       "qt.qpa.events.*.debug=false\n"
                       "qt.qpa.screen.*.debug=false\n"
                       "qt.qml.*.debug=false"));

    if (isJournalctlAvailable()) {
        // journald is available, don't install a custom handler
        // Qt's default behavior sends to stderr, which journald captures
        return;
    }
    qInstallMessageHandler(fileMessageHandler);
}
