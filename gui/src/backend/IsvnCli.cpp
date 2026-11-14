#include "backend/IsvnCli.h"

#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

#include <memory>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace isvn::gui {
namespace {

constexpr int kDefaultTimeoutMs = 30000;
constexpr qsizetype kMaxRequestStdinBytes = 8 * 1024 * 1024;
constexpr int kReadLaneLimit = 1;

void terminateProcessTree(QProcess* process, int signal_number) {
    if (process == nullptr || process->state() == QProcess::NotRunning) return;
#ifdef Q_OS_UNIX
    const qint64 pid = process->processId();
    if (pid > 0 && ::kill(-static_cast<pid_t>(pid), signal_number) == 0) return;
#else
    Q_UNUSED(signal_number)
#endif
    if (signal_number == SIGKILL) process->kill();
    else process->terminate();
}

}  // namespace

IsvnCli::IsvnCli(QObject* parent) : QObject(parent) {}

QString IsvnCli::executable() const { return executable_; }

void IsvnCli::setExecutable(const QString& executable) {
    if (executable_ == executable) return;
    executable_ = executable;
    emit executableChanged();
}

bool IsvnCli::running() const { return running_; }
QString IsvnCli::lastError() const { return last_error_; }
QVariant IsvnCli::lastJson() const { return last_json_; }

QString IsvnCli::maintenanceRole() const { return maintenance_role_; }
int IsvnCli::activeMutations() const { return active_mutations_; }
int IsvnCli::activeReads() const { return active_reads_; }

void IsvnCli::setMaintenanceRole(const QString& role) {
    if (maintenance_role_ == role) return;
    maintenance_role_ = role;
    emit maintenanceRoleChanged();
}

// static
bool IsvnCli::isMutatingMaintenanceCommand(const QStringList& arguments) {
    if (arguments.isEmpty()) return false;
    if (arguments.first() == QStringLiteral("gc") &&
        !arguments.contains(QStringLiteral("--dry-run")))
        return true;
    return false;
}

QStringList IsvnCli::jsonArguments(QStringList arguments) const {
    if (!arguments.contains(QStringLiteral("--json"))) {
        arguments.push_back(QStringLiteral("--json"));
    }
    return arguments;
}

QVariant IsvnCli::parseJsonOutput(const QByteArray& output, QString* error) const {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(output, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("CLI JSON parse failed: %1").arg(parse_error.errorString());
        }
        return {};
    }
    return document.toVariant();
}

QString IsvnCli::processErrorMessage(QProcess* process, const QByteArray& stderr_text) const {
    if (process->error() == QProcess::FailedToStart) {
        return QStringLiteral("找不到 isvn，請確認已安裝且在 PATH");
    }
    const QString stderr_message = QString::fromLocal8Bit(stderr_text).trimmed();
    if (stderr_message.contains(QStringLiteral("Permission denied"), Qt::CaseInsensitive) ||
        stderr_message.contains(QStringLiteral("permission"), Qt::CaseInsensitive)) {
        return QStringLiteral("權限不足：%1").arg(stderr_message);
    }
    if (!stderr_message.isEmpty()) return stderr_message;
    return process->errorString();
}

void IsvnCli::runJson(const QStringList& arguments) {
    runJsonInternal(arguments, {});
}

void IsvnCli::runJsonWithStdin(const QStringList& arguments, const QString& requestJson) {
    const QByteArray payload = requestJson.toUtf8();
    if (payload.isEmpty() || payload.size() > kMaxRequestStdinBytes) {
        const QString message = QStringLiteral("import request is too large or empty");
        setLastError(message);
        emit commandFailed(message);
        return;
    }
    runJsonInternal(arguments, payload);
}

QVariant IsvnCli::structuredError(const QString& code, bool retryable) const {
    return QVariantMap{{QStringLiteral("schema_version"), QStringLiteral("work-error/v1")},
                       {QStringLiteral("ok"), false},
                       {QStringLiteral("operation_id"), QVariant()},
                       {QStringLiteral("error"),
                        QVariantMap{{QStringLiteral("code"), code},
                                    {QStringLiteral("reason_params"), QVariantMap{}},
                                    {QStringLiteral("retryable"), retryable},
                                    {QStringLiteral("recovery_required"), false}}}};
}

bool IsvnCli::runJsonForOperation(const QString& operationId, const QString& targetVersion,
                                  const QVariant& arguments, const QString& requestJson,
                                  bool mutation) {
    QStringList args;
    for (const QVariant& value : arguments.toList()) args.push_back(value.toString());
    const QByteArray stdinPayload = requestJson.toUtf8();
    if (operationId.isEmpty() || targetVersion.isEmpty() || args.isEmpty()) {
        const QVariant error = structuredError(QStringLiteral("OPERATION_TARGET_DIVERGED"), false);
        emit operationFailedJson(operationId, targetVersion, error);
        emit operationFailed(operationId, targetVersion, QStringLiteral("invalid operation context"));
        return false;
    }
    if (!stdinPayload.isEmpty() && stdinPayload.size() > kMaxRequestStdinBytes) {
        const QVariant error = structuredError(QStringLiteral("REQUEST_LIMIT_EXCEEDED"), false);
        emit operationFailedJson(operationId, targetVersion, error);
        emit operationFailed(operationId, targetVersion, QStringLiteral("bounded request too large"));
        return false;
    }
    return runOperationInternal(operationId, targetVersion, args, stdinPayload, mutation);
}

void IsvnCli::runJsonInternal(const QStringList& arguments, const QByteArray& stdinPayload) {
    // Backend-level guard: mutating maintenance commands require PM role.
    // maintenance_role_ is wired from the QML roleResolver. An empty role
    // (not yet resolved) passes through so startup diagnostics are not broken.
    if (isMutatingMaintenanceCommand(arguments) &&
        !maintenance_role_.isEmpty() &&
        maintenance_role_ != QStringLiteral("PM")) {
        const QString message =
            QStringLiteral("maintenance-auth-denied: role=") + maintenance_role_;
        setLastError(message);
        emit commandFailed(message);
        return;
    }

    if (legacy_running_) {
        setLastError(QStringLiteral("已有 isvn 指令正在執行"));
        emit commandFailed(last_error_);
        return;
    }

    auto* process = new QProcess(this);
#ifdef Q_OS_UNIX
    // Each GUI operation owns a child process group.  Force Exit can therefore
    // terminate the CLI and any inherited helper child without touching an
    // unrelated GUI operation or relying on a shell command line.
    process->setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
    const QStringList final_arguments = jsonArguments(arguments);
    legacy_running_ = true;
    setRunning(true);
    setLastError(QString());

    connect(process, &QProcess::finished, this,
            [this, process](int exit_code, QProcess::ExitStatus exit_status) {
                const QByteArray stdout_text = process->readAllStandardOutput();
                const QByteArray stderr_text = process->readAllStandardError();
                legacy_running_ = false;
                setRunning(active_mutations_ != 0 || active_reads_ != 0);
                if (exit_status != QProcess::NormalExit || exit_code != 0) {
// Prefer a structured JSON error envelope on stdout.
                    // over raw stderr, so the Work UI can map a stable error code.
                    QString structured_parse_error;
                    const QVariant structured =
                        parseJsonOutput(stdout_text, &structured_parse_error);
                    if (structured_parse_error.isEmpty() &&
                        structured.canConvert<QVariantMap>() &&
                        !structured.toMap().isEmpty()) {
                        setLastJson(structured);
                        emit commandFailedJson(structured);
                        const QString code = structured.toMap()
                                                 .value(QStringLiteral("error"))
                                                 .toMap()
                                                 .value(QStringLiteral("code"))
                                                 .toString();
                        const QString message =
                            code.isEmpty() ? QStringLiteral("operation failed")
                                           : QStringLiteral("operation failed: %1").arg(code);
                        setLastError(message);
                        emit commandFailed(message);
                        process->deleteLater();
                        return;
                    }
                    const QString message = processErrorMessage(process, stderr_text);
                    setLastError(message);
                    emit commandFailed(message);
                    process->deleteLater();
                    return;
                }
                QString parse_error;
                const QVariant json = parseJsonOutput(stdout_text, &parse_error);
                if (!parse_error.isEmpty()) {
                    setLastError(parse_error);
                    emit commandFailed(parse_error);
                    process->deleteLater();
                    return;
                }
                setLastJson(json);
                emit commandSucceeded(json);
                process->deleteLater();
            });

    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
        if (process->state() != QProcess::NotRunning) return;
        legacy_running_ = false;
        setRunning(active_mutations_ != 0 || active_reads_ != 0);
        const QString message = processErrorMessage(process, process->readAllStandardError());
        setLastError(message);
        emit commandFailed(message);
        process->deleteLater();
    });

    if (!stdinPayload.isEmpty()) {
        // QProcess owns its write buffer.  Queue every byte (including a rare
        // short write) before closing the channel, so an Import request is
        // finite and the CLI can make an EOF-based size decision.
        connect(process, &QProcess::started, this, [this, process, stdinPayload]() {
            qint64 offset = 0;
            while (offset < stdinPayload.size()) {
                const qint64 accepted = process->write(stdinPayload.constData() + offset,
                                                       stdinPayload.size() - offset);
                if (accepted <= 0) {
                    process->kill();
                    setLastError(QStringLiteral("unable to send bounded import request"));
                    emit commandFailed(last_error_);
                    return;
                }
                offset += accepted;
            }
            process->closeWriteChannel();
        });
    }
    process->start(executable_, final_arguments);
}

bool IsvnCli::runOperationInternal(const QString& operationId, const QString& targetVersion,
                                   const QStringList& arguments, const QByteArray& stdinPayload,
                                   bool mutation) {
    // A caller cannot label an obvious mutating command as a read: mutations
    // are counted separately and are never dropped the way an overlapping read
    // is.  Mutations are not limited or serialized here; each controller keeps
    // its own in-flight state.
    const QString command = arguments.value(0);
    const bool versionApply = (command == QStringLiteral("import") ||
                               command == QStringLiteral("checkin-batch") ||
                               command == QStringLiteral("remove-batch") ||
                               command == QStringLiteral("work-local-op")) &&
                              arguments.contains(QStringLiteral("apply"));
    const bool recoveryMutation = command == QStringLiteral("work-recovery") &&
                                  (arguments.contains(QStringLiteral("restore")) ||
                                   arguments.contains(QStringLiteral("verify")));
    const bool explicitMutation = versionApply || recoveryMutation ||
                                  command == QStringLiteral("create") ||
                                  command == QStringLiteral("checkout") ||
                                  command == QStringLiteral("gc") ||
                                  command == QStringLiteral("reconcile");
    const bool isMutation = mutation || explicitMutation;
    if (isMutatingMaintenanceCommand(arguments) && !maintenance_role_.isEmpty() &&
        maintenance_role_ != QStringLiteral("PM")) {
        const QVariant error = structuredError(QStringLiteral("AUTHORIZATION_DENIED"), false);
        emit operationFailedJson(operationId, targetVersion, error);
        emit operationFailed(operationId, targetVersion, QStringLiteral("operation denied"));
        return false;
    }
    if (!isMutation && active_reads_ >= kReadLaneLimit) {
        const QVariant error = structuredError(QStringLiteral("READ_LANE_BUSY"));
        emit operationFailedJson(operationId, targetVersion, error);
        emit operationFailed(operationId, targetVersion, QStringLiteral("read lane busy"));
        return false;
    }

    auto* process = new QProcess(this);
#ifdef Q_OS_UNIX
    // Keep each admitted operation (not only legacy calls) in its own group so
    // a bounded Force Exit reaches any helper inherited by the CLI process.
    process->setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
    operation_processes_.insert(process);
    if (isMutation) ++active_mutations_;
    else ++active_reads_;
    emit operationLaneChanged();
    setRunning(true);
    const QStringList finalArguments = jsonArguments(arguments);
    const auto released = std::make_shared<bool>(false);
    auto release = [this, isMutation, released]() {
        if (*released) return;
        *released = true;
        if (isMutation) --active_mutations_;
        else --active_reads_;
        emit operationLaneChanged();
        setRunning(legacy_running_ || active_mutations_ != 0 || active_reads_ != 0);
    };
    connect(process, &QProcess::finished, this,
            [this, process, operationId, targetVersion, release](int exitCode,
                                                                   QProcess::ExitStatus exitStatus) {
                const QByteArray stdoutText = process->readAllStandardOutput();
                release();
                operation_processes_.remove(process);
                if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                    QString parseError;
                    const QVariant structured = parseJsonOutput(stdoutText, &parseError);
                    if (parseError.isEmpty() && structured.canConvert<QVariantMap>() &&
                        !structured.toMap().isEmpty()) {
                        emit operationFailedJson(operationId, targetVersion, structured);
                    } else {
                        emit operationFailedJson(operationId, targetVersion,
                                                 structuredError(QStringLiteral("SVN_QUERY_FAILED"), false));
                    }
                    emit operationFailed(operationId, targetVersion, QStringLiteral("operation failed"));
                    process->deleteLater();
                    return;
                }
                QString parseError;
                const QVariant json = parseJsonOutput(stdoutText, &parseError);
                if (!parseError.isEmpty()) {
                    emit operationFailedJson(operationId, targetVersion,
                                             structuredError(QStringLiteral("SVN_QUERY_FAILED"), false));
                    emit operationFailed(operationId, targetVersion, QStringLiteral("CLI JSON parse failed"));
                } else {
                    emit operationSucceeded(operationId, targetVersion, json);
                }
                process->deleteLater();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, operationId, targetVersion, release](QProcess::ProcessError) {
                if (process->state() != QProcess::NotRunning) return;
                release();
                operation_processes_.remove(process);
                emit operationFailedJson(operationId, targetVersion,
                                         structuredError(QStringLiteral("SVN_QUERY_FAILED"), false));
                emit operationFailed(operationId, targetVersion, QStringLiteral("operation failed"));
                process->deleteLater();
            });
    if (!stdinPayload.isEmpty()) {
        connect(process, &QProcess::started, this, [process, stdinPayload]() {
            qint64 offset = 0;
            while (offset < stdinPayload.size()) {
                const qint64 accepted = process->write(stdinPayload.constData() + offset,
                                                       stdinPayload.size() - offset);
                if (accepted <= 0) { process->kill(); return; }
                offset += accepted;
            }
            process->closeWriteChannel();
        });
    }
    process->start(executable_, finalArguments);
    return true;
}

void IsvnCli::requestBoundedShutdown(int grace_ms) {
    const int bounded_grace = qBound(1, grace_ms, 1000);
    const auto processes = operation_processes_.values();
    for (QProcess* process : processes) {
        terminateProcessTree(process, SIGTERM);
    }
    QTimer::singleShot(bounded_grace, this, [this]() {
        const auto remaining = operation_processes_.values();
        for (QProcess* process : remaining) {
            terminateProcessTree(process, SIGKILL);
        }
    });
}

QVariant IsvnCli::runJsonBlocking(const QStringList& arguments, int timeout_ms) {
    QProcess process;
    process.start(executable_, jsonArguments(arguments));
    const int timeout = timeout_ms <= 0 ? kDefaultTimeoutMs : timeout_ms;
    if (!process.waitForStarted(timeout)) {
        const QString message = processErrorMessage(&process, process.readAllStandardError());
        setLastError(message);
        return {};
    }
    if (!process.waitForFinished(timeout)) {
        process.kill();
        process.waitForFinished();
        setLastError(QStringLiteral("isvn 指令逾時"));
        return {};
    }
    const QByteArray stdout_text = process.readAllStandardOutput();
    const QByteArray stderr_text = process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        setLastError(processErrorMessage(&process, stderr_text));
        return {};
    }
    QString parse_error;
    const QVariant json = parseJsonOutput(stdout_text, &parse_error);
    if (!parse_error.isEmpty()) {
        setLastError(parse_error);
        return {};
    }
    setLastError(QString());
    setLastJson(json);
    return json;
}

void IsvnCli::setRunning(bool running) {
    if (running_ == running) return;
    running_ = running;
    emit runningChanged();
}

void IsvnCli::setLastError(const QString& message) {
    if (last_error_ == message) return;
    last_error_ = message;
    emit lastErrorChanged();
}

void IsvnCli::setLastJson(const QVariant& json) {
    last_json_ = json;
    emit lastJsonChanged();
}

}  // namespace isvn::gui
