#include "backend/RoleResolver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

namespace isvn::gui {
namespace {

QString trimValue(QString value) { return value.trimmed(); }

}  // namespace

RoleResolver::RoleResolver(QObject* parent) : QObject(parent), current_user_(effectiveUserName()) {}

QString RoleResolver::roleName() const { return role_name_; }
bool RoleResolver::allowed() const { return allowed_; }
QString RoleResolver::message() const { return message_; }
QString RoleResolver::currentUser() const { return current_user_; }
QString RoleResolver::projectDir() const { return project_dir_; }
QString RoleResolver::projectGroup() const { return project_group_; }

QVariantMap RoleResolver::refresh(const QString& start_path) { return resolve(start_path); }

QVariantMap RoleResolver::resolve(const QString& start_path) {
    QString error;
    const QString project_dir = findProjectDir(start_path, &error);
    if (project_dir.isEmpty()) {
        return setResult(QStringLiteral("DENIED"), false, error, QString(), QString());
    }

    const QString isvn_dir = QDir(project_dir).filePath(QStringLiteral(".isvn"));
    struct stat status {};
    if (::stat(isvn_dir.toLocal8Bit().constData(), &status) != 0) {
        return setResult(QStringLiteral("DENIED"), false,
                         QStringLiteral("無法讀取 .isvn 權限"), project_dir, QString());
    }

    const QString project_group = readProjectGroup(project_dir);
    current_user_ = effectiveUserName();
    if (status.st_uid == ::geteuid()) {
        return setResult(QStringLiteral("PM"), true, QStringLiteral("PM"), project_dir,
                         project_group);
    }
    if (!project_group.isEmpty() && currentUserInGroup(project_group)) {
        return setResult(QStringLiteral("MEMBER"), true, QStringLiteral("MEMBER"), project_dir,
                         project_group);
    }
    return setResult(QStringLiteral("DENIED"), false, QStringLiteral("目前使用者不屬於專案 group"),
                     project_dir, project_group);
}

QVariantMap RoleResolver::setResult(const QString& role, bool allowed, const QString& message,
                                    const QString& project_dir,
                                    const QString& project_group) {
    role_name_ = role;
    allowed_ = allowed;
    message_ = message;
    project_dir_ = project_dir;
    project_group_ = project_group;
    QVariantMap result;
    result.insert(QStringLiteral("role"), role_name_);
    result.insert(QStringLiteral("allowed"), allowed_);
    result.insert(QStringLiteral("message"), message_);
    result.insert(QStringLiteral("currentUser"), current_user_);
    result.insert(QStringLiteral("projectDir"), project_dir_);
    result.insert(QStringLiteral("projectGroup"), project_group_);
    emit roleChanged();
    return result;
}

QString RoleResolver::findProjectDir(const QString& start_path, QString* error) const {
    QDir current(QFileInfo(start_path).absoluteFilePath());
    if (!current.exists()) current = QFileInfo(start_path).absoluteDir();
    while (true) {
        if (current.exists(QStringLiteral(".isvn"))) return current.absolutePath();
        if (current.exists(QStringLiteral(".isvn_local/config"))) {
            return readProjectDirFromLocalConfig(current.filePath(QStringLiteral(".isvn_local/config")));
        }
        if (!current.cdUp()) break;
    }
    if (error != nullptr) {
        *error = QStringLiteral("找不到 isvn 專案；請在 workspace 或 project_dir 內啟動");
    }
    return {};
}

QString RoleResolver::readProjectDirFromLocalConfig(const QString& local_config) const {
    QFile file(local_config);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals < 0) continue;
        if (trimValue(line.left(equals)) == QStringLiteral("project_dir")) {
            return QFileInfo(trimValue(line.mid(equals + 1))).absoluteFilePath();
        }
    }
    return {};
}

QString RoleResolver::readProjectGroup(const QString& project_dir) const {
    QFile file(QDir(project_dir).filePath(QStringLiteral(".isvn/config")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals < 0) continue;
        if (trimValue(line.left(equals)) == QStringLiteral("project_group")) {
            return trimValue(line.mid(equals + 1));
        }
    }
    return {};
}

bool RoleResolver::currentUserInGroup(const QString& group_name) const {
    const QByteArray group_bytes = group_name.toLocal8Bit();
    const group* entry = ::getgrnam(group_bytes.constData());
    if (entry == nullptr) return false;
    const gid_t target = entry->gr_gid;
    if (::getgid() == target || ::getegid() == target) return true;
    const int count = ::getgroups(0, nullptr);
    if (count <= 0) return false;
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    if (::getgroups(count, groups.data()) < 0) return false;
    return std::find(groups.begin(), groups.end(), target) != groups.end();
}

QString RoleResolver::effectiveUserName() const {
    const passwd* entry = ::getpwuid(::geteuid());
    if (entry == nullptr || entry->pw_name == nullptr) return QString::number(::geteuid());
    return QString::fromLocal8Bit(entry->pw_name);
}

}  // namespace isvn::gui
