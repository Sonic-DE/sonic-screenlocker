/*
SPDX-FileCopyrightText: 2026

SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QDateTime>
#include <QFile>
#include <QFileDevice>
#include <QMessageLogger>
#include <QStandardPaths>
#include <QTextStream>

#include <sys/stat.h>

static const QString s_logFilePath = QStringLiteral("/var/log/sonic/screenlocker.log");
static const QString s_logFileBackupPath = QStringLiteral("/var/log/sonic/screenlocker.log.last");

static bool s_logRotationDone = false;

static void rotateLogFile()
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

static bool isJournalctlAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("journalctl")).isEmpty();
}

static void ensureLogFileExists()
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

static void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    ensureLogFileExists();
    QFile logFile(s_logFilePath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&logFile);
        const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        const char *typeStr = "";
        switch (type) {
        case QtDebugMsg:
            typeStr = "DEBUG";
            break;
        case QtInfoMsg:
            typeStr = "INFO";
            break;
        case QtWarningMsg:
            typeStr = "WARNING";
            break;
        case QtCriticalMsg:
            typeStr = "CRITICAL";
            break;
        case QtFatalMsg:
            typeStr = "FATAL";
            break;
        }
        const QString category = context.category ? QStringLiteral("%1").arg(context.category) : QStringLiteral("unknown");
        out << timestamp << " [" << typeStr << "] [" << category << "] " << msg << "\n";
        out.flush();
        logFile.close();
    }
}

static void installFileLogger()
{
    // Rotate log file once per application run
    rotateLogFile();

    if (isJournalctlAvailable()) {
        // journald is available, don't install a custom handler
        // Qt's default behavior sends to stderr, which journald captures
        return;
    }
    qInstallMessageHandler(fileMessageHandler);
}
