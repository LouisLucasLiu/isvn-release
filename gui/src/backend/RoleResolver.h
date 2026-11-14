#pragma once

#include <QObject>
#include <QVariant>

namespace isvn::gui {

class RoleResolver : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString roleName READ roleName NOTIFY roleChanged)
    Q_PROPERTY(bool allowed READ allowed NOTIFY roleChanged)
    Q_PROPERTY(QString message READ message NOTIFY roleChanged)
    Q_PROPERTY(QString currentUser READ currentUser NOTIFY roleChanged)
    Q_PROPERTY(QString projectDir READ projectDir NOTIFY roleChanged)
    Q_PROPERTY(QString projectGroup READ projectGroup NOTIFY roleChanged)

public:
    explicit RoleResolver(QObject* parent = nullptr);

    QString roleName() const;
    bool allowed() const;
    QString message() const;
    QString currentUser() const;
    QString projectDir() const;
    QString projectGroup() const;

    Q_INVOKABLE QVariantMap resolve(const QString& start_path);
    Q_INVOKABLE QVariantMap refresh(const QString& start_path = QStringLiteral("."));

signals:
    void roleChanged();

private:
    QVariantMap setResult(const QString& role, bool allowed, const QString& message,
                          const QString& project_dir, const QString& project_group);
    QString findProjectDir(const QString& start_path, QString* error) const;
    QString readProjectDirFromLocalConfig(const QString& local_config) const;
    QString readProjectGroup(const QString& project_dir) const;
    bool currentUserInGroup(const QString& group_name) const;
    QString effectiveUserName() const;

    QString role_name_ = QStringLiteral("UNKNOWN");
    bool allowed_ = false;
    QString message_;
    QString current_user_;
    QString project_dir_;
    QString project_group_;
};

}  // namespace isvn::gui
