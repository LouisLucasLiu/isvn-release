#pragma once

#include <QObject>
#include <QSet>
#include <QVariant>

class QProcess;

namespace isvn::gui {

class IsvnCli : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString executable READ executable WRITE setExecutable NOTIFY executableChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVariant lastJson READ lastJson NOTIFY lastJsonChanged)
    Q_PROPERTY(QString maintenanceRole READ maintenanceRole WRITE setMaintenanceRole NOTIFY maintenanceRoleChanged)
    Q_PROPERTY(int activeMutations READ activeMutations NOTIFY operationLaneChanged)
    Q_PROPERTY(int activeReads READ activeReads NOTIFY operationLaneChanged)

public:
    explicit IsvnCli(QObject* parent = nullptr);

    QString executable() const;
    void setExecutable(const QString& executable);

    bool running() const;
    QString lastError() const;
    QVariant lastJson() const;

    QString maintenanceRole() const;
    void setMaintenanceRole(const QString& role);
    int activeMutations() const;
    int activeReads() const;

    Q_INVOKABLE void runJson(const QStringList& arguments);
    // External Import request payload is sent only over stdin. The
    // request never becomes part of argv, Activity Log, or a command display.
    Q_INVOKABLE void runJsonWithStdin(const QStringList& arguments, const QString& requestJson);
    Q_INVOKABLE QVariant runJsonBlocking(const QStringList& arguments, int timeout_ms = 30000);
    // Target-pinned lane. Unlike the legacy single-flight signals,
    // every completion includes an immutable operation ID and target Version.
    // `arguments` is QVariant for direct QML array calls; external source
    // request data stays solely in the bounded stdin payload.
    Q_INVOKABLE bool runJsonForOperation(const QString& operationId,
                                         const QString& targetVersion,
                                         const QVariant& arguments,
                                         const QString& requestJson = QString(),
                                         bool mutation = true);
    // GUI Force Exit only requests bounded child shutdown.  Durable operation
    // journals remain backend evidence; this method never asserts rollback or
    // outcome attribution.
    Q_INVOKABLE void requestBoundedShutdown(int grace_ms = 250);

signals:
    void executableChanged();
    void runningChanged();
    void lastErrorChanged();
    void lastJsonChanged();
    void maintenanceRoleChanged();
    void commandSucceeded(const QVariant& json);
    void commandFailed(const QString& message);
    // Emitted (in addition to commandFailed) when a command exits nonzero but
    // still prints a valid structured JSON envelope on stdout.
    // work-error/v1). Lets the Work action controller read {error.code, ...}
    // instead of surfacing raw stderr.
    void commandFailedJson(const QVariant& json);
    void operationLaneChanged();
    void operationSucceeded(const QString& operationId, const QString& targetVersion,
                            const QVariant& json);
    void operationFailedJson(const QString& operationId, const QString& targetVersion,
                             const QVariant& json);
    void operationFailed(const QString& operationId, const QString& targetVersion,
                         const QString& message);

private:
    static bool isMutatingMaintenanceCommand(const QStringList& arguments);
    QStringList jsonArguments(QStringList arguments) const;
    QVariant parseJsonOutput(const QByteArray& output, QString* error) const;
    QString processErrorMessage(QProcess* process, const QByteArray& stderr_text) const;
    void runJsonInternal(const QStringList& arguments, const QByteArray& stdinPayload);
    bool runOperationInternal(const QString& operationId, const QString& targetVersion,
                              const QStringList& arguments, const QByteArray& stdinPayload,
                              bool mutation);
    QVariant structuredError(const QString& code, bool retryable = true) const;
    void setRunning(bool running);
    void setLastError(const QString& message);
    void setLastJson(const QVariant& json);

    QString executable_ = QStringLiteral("isvn");
    bool running_ = false;
    QString last_error_;
    QVariant last_json_;
    QString maintenance_role_;
    bool legacy_running_ = false;
    int active_mutations_ = 0;
    int active_reads_ = 0;
    QSet<QProcess*> operation_processes_;
};

}  // namespace isvn::gui
