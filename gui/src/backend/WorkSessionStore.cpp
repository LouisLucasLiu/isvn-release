#include "backend/WorkSessionStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>

#include <memory>

namespace isvn::gui {
namespace {

constexpr const char* kSchema = "work-session/v1";
constexpr int kSettingsWriterTimeoutMs = 250;

// Deterministic, non-reversible-enough scope key from the raw project path, so
// the absolute project path is not persisted as a settings key.
QString hashProjectKey(const QString& key) {
    std::uint64_t hash = 1469598103934665603ULL;
    const QByteArray bytes = key.toUtf8();
    for (const unsigned char c : bytes) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return QStringLiteral("p_%1").arg(hash, 16, 16, QLatin1Char('0'));
}

QString currentUser() {
    QByteArray user = qgetenv("USER");
    if (user.isEmpty()) user = qgetenv("LOGNAME");
    return user.isEmpty() ? QStringLiteral("unknown") : QString::fromUtf8(user);
}

const QStringList& allowlist() {
    static const QStringList keys{
        QStringLiteral("operationId"),   QStringLiteral("operationType"),
        QStringLiteral("target"),        QStringLiteral("startTime"),
        QStringLiteral("endTime"),       QStringLiteral("user"),
        QStringLiteral("role"),          QStringLiteral("status"),
        QStringLiteral("revision"),      QStringLiteral("summary"),
        QStringLiteral("buildIdentity"), QStringLiteral("itemCount")};
    return keys;
}

bool looksLikeAbsoluteOrExternal(const QString& value) {
    return value.startsWith(QLatin1Char('/')) || value.contains(QStringLiteral("://")) ||
           value.contains(QStringLiteral("\\")) ||
           QRegularExpression(QStringLiteral("(^|[\\s:=\\(])/(?:[^\\s]+)")).match(value).hasMatch();
}

}  // namespace

WorkSessionStore::WorkSessionStore(QObject* parent) : QObject(parent) {
    QCoreApplication::setOrganizationName(QStringLiteral("isvn"));
    QCoreApplication::setApplicationName(QStringLiteral("isvn-gui"));
}

QString WorkSessionStore::projectKey() const { return project_key_; }

void WorkSessionStore::setProjectKey(const QString& key) {
    if (project_key_ == key) return;
    project_key_ = key;
    emit projectKeyChanged();
    emit activeChanged();
    emit activityChanged();
}

QString WorkSessionStore::storageFile() const { return storage_file_; }

void WorkSessionStore::setStorageFile(const QString& path) {
    if (storage_file_ == path) return;
    storage_file_ = path;
    emit storageFileChanged();
}

QString WorkSessionStore::scopeHash() const {
    return currentUser() + QStringLiteral("/") + hashProjectKey(project_key_);
}

QString WorkSessionStore::group() const {
    return QStringLiteral("work/") + scopeHash();
}

namespace {
std::unique_ptr<QSettings> makeSettings(const QString& storage_file) {
    if (!storage_file.isEmpty()) {
        return std::make_unique<QSettings>(storage_file, QSettings::IniFormat);
    }
    return std::make_unique<QSettings>();
}

// QSettings serializes individual sync operations, but a read/modify/write
// Activity append still needs one short writer critical section.  This lock is
// only for GUI-local convenience state; backend journals/locks remain the
// operation authority.  A bounded failure preserves the old complete INI
// rather than silently dropping another writer's update.
std::unique_ptr<QLockFile> acquireSettingsWriterLock(const QSettings& settings) {
    const QString filename = settings.fileName();
    if (filename.isEmpty()) return {};
    // In a new account the settings directory does not exist yet, and the lock
    // file is created before the first settings file is written.
    QDir().mkpath(QFileInfo(filename).absolutePath());
    auto lock = std::make_unique<QLockFile>(filename + QStringLiteral(".work-session.lock"));
    lock->setStaleLockTime(30000);
    if (!lock->tryLock(kSettingsWriterTimeoutMs)) return {};
    return lock;
}
}  // namespace

void WorkSessionStore::setActiveVersion(const QString& workspaceId, const QString& version) {
    if (workspaceId.isEmpty() || version.isEmpty()) return;
    auto settings = makeSettings(storage_file_);
    auto lock = acquireSettingsWriterLock(*settings);
    if (!lock) {
        emit warning(QStringLiteral("work session writer is busy"));
        return;
    }
    settings->beginGroup(group());
    settings->setValue(QStringLiteral("schema"), QString::fromLatin1(kSchema));
    settings->setValue(QStringLiteral("active/workspace_id"), workspaceId);
    settings->setValue(QStringLiteral("active/version"), version);
    settings->endGroup();
    settings->sync();
    emit activeChanged();
}

QVariantMap WorkSessionStore::activeHint() const {
    auto settings = makeSettings(storage_file_);
    settings->beginGroup(group());
    const QString schema = settings->value(QStringLiteral("schema")).toString();
    const QString workspace_id = settings->value(QStringLiteral("active/workspace_id")).toString();
    const QString version = settings->value(QStringLiteral("active/version")).toString();
    settings->endGroup();
    if (schema != QString::fromLatin1(kSchema) || workspace_id.isEmpty() || version.isEmpty()) {
        return {};
    }
    return QVariantMap{{QStringLiteral("workspace_id"), workspace_id},
                      {QStringLiteral("version"), version}};
}

void WorkSessionStore::clearActiveVersion() {
    auto settings = makeSettings(storage_file_);
    auto lock = acquireSettingsWriterLock(*settings);
    if (!lock) {
        emit warning(QStringLiteral("work session writer is busy"));
        return;
    }
    settings->beginGroup(group());
    settings->remove(QStringLiteral("active/workspace_id"));
    settings->remove(QStringLiteral("active/version"));
    settings->endGroup();
    settings->sync();
    emit activeChanged();
}

QVariantMap WorkSessionStore::sanitizeActivity(const QVariantMap& record, QString& rejected) {
    rejected.clear();
    const QString operation_id = record.value(QStringLiteral("operationId")).toString();
    const QString operation_type = record.value(QStringLiteral("operationType")).toString();
    if (operation_id.isEmpty() || operation_type.isEmpty()) {
        rejected = QStringLiteral("operationId and operationType are required");
        return {};
    }
    const QString target = record.value(QStringLiteral("target")).toString();
    if (looksLikeAbsoluteOrExternal(target)) {
        rejected = QStringLiteral("target must be project-relative, not an absolute/external path");
        return {};
    }
    QVariantMap clean;
    for (const QString& key : allowlist()) {
        if (!record.contains(key)) continue;
        const QString value = record.value(key).toString();
        // Defense in depth: never persist an absolute/external path in any field.
        if (looksLikeAbsoluteOrExternal(value)) continue;
        clean.insert(key, value);
    }
    return clean;
}

bool WorkSessionStore::appendActivity(const QVariantMap& record) {
    QString rejected;
    const QVariantMap clean = sanitizeActivity(record, rejected);
    if (clean.isEmpty()) {
        emit warning(QStringLiteral("activity record rejected: ") + rejected);
        return false;
    }
    auto settings = makeSettings(storage_file_);
    auto lock = acquireSettingsWriterLock(*settings);
    if (!lock) {
        emit warning(QStringLiteral("work session writer is busy"));
        return false;
    }
    settings->beginGroup(group());
    const QString raw = settings->value(QStringLiteral("activity/records")).toString();
    settings->endGroup();

    QJsonArray records;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (doc.isArray()) records = doc.array();

    QJsonObject obj;
    for (auto it = clean.constBegin(); it != clean.constEnd(); ++it) {
        obj.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }
    // One record per operation: a later record for the same operation replaces
    // the earlier one instead of being listed beside it.
    const QString operation_id = clean.value(QStringLiteral("operationId")).toString();
    for (int i = records.size() - 1; i >= 0; --i) {
        if (records.at(i).toObject().value(QStringLiteral("operationId")).toString() ==
            operation_id) {
            records.removeAt(i);
        }
    }
    records.prepend(obj);  // newest first
    while (records.size() > activityLimit()) {
        records.removeLast();  // deterministic eviction of oldest
    }

    settings->beginGroup(group());
    settings->setValue(QStringLiteral("schema"), QString::fromLatin1(kSchema));
    settings->setValue(QStringLiteral("activity/records"),
                       QString::fromUtf8(QJsonDocument(records).toJson(QJsonDocument::Compact)));
    settings->endGroup();
    settings->sync();
    emit activityChanged();
    return true;
}

QVariantList WorkSessionStore::recentActivity() const {
    auto settings = makeSettings(storage_file_);
    settings->beginGroup(group());
    const QString schema = settings->value(QStringLiteral("schema")).toString();
    const QString raw = settings->value(QStringLiteral("activity/records")).toString();
    settings->endGroup();
    QVariantList result;
    if (schema != QString::fromLatin1(kSchema)) {
        return result;  // corrupt/unsupported schema -> empty, no crash
    }
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isArray()) return result;
    for (const QJsonValue& value : doc.array()) {
        if (value.isObject()) result.append(value.toObject().toVariantMap());
    }
    return result;
}

void WorkSessionStore::resetWorkSession() {
    auto settings = makeSettings(storage_file_);
    auto lock = acquireSettingsWriterLock(*settings);
    if (!lock) {
        emit warning(QStringLiteral("work session writer is busy"));
        return;
    }
    settings->beginGroup(group());
    settings->remove(QString());  // clears only this scope's group
    settings->endGroup();
    settings->sync();
    emit activeChanged();
    emit activityChanged();
}

}  // namespace isvn::gui
