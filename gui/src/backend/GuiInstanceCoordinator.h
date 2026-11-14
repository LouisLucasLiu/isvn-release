#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace isvn::gui {

// GuiInstanceCoordinator is deliberately a GUI-process concern: it never
// reads project data or starts an operation.  It owns a private, per-user
// activation endpoint so two shells cannot independently operate on one
// Project.  The endpoint only carries a schema and shell mode.
class GuiInstanceCoordinator final : public QObject {
    Q_OBJECT

public:
    enum class AcquireResult { Owner, ActivatedExisting, ModeConflict, Error };
    Q_ENUM(AcquireResult)

    explicit GuiInstanceCoordinator(QObject* parent = nullptr);
    ~GuiInstanceCoordinator() override;

    AcquireResult acquire(const QString& project_identity, const QString& shell_mode,
                          QString* error = nullptr);
    QString endpointName() const;

signals:
    void activationRequested();

private:
    bool startOwner(const QString& endpoint, const QString& shell_mode, QString* error);
    AcquireResult activateExisting(const QString& endpoint, const QString& shell_mode,
                                  QString* error) const;
    static QString endpointFor(const QString& project_identity);
    static bool ensureRuntimeDirectory(QString* error);

    QLocalServer* server_ = nullptr;
    QString endpoint_;
    QString shell_mode_;
};

}  // namespace isvn::gui
