#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace isvn::gui {

// WorkSessionStore — bounded, GUI-local session state for the Work screen.
// It persists, scoped by user + fixed project:
//   - the last Active Working Version hint (validated read-only at startup)
//   - a bounded (<=200) local Activity Log of allowlisted, sanitized records
//
// It is LOCAL convenience/evidence state only. It is NOT a repository authority,
// does not replace SVN history, and never bypasses IsvnCli for product data. It
// never stores Explore Inspected Version as Active, and never persists absolute
// external paths, raw command lines, environment, credentials, or raw backend
// output.
//
// Storage uses QSettings. Tests point `storageFile` at a temporary INI file so
// they never read or write the real user profile.
class WorkSessionStore : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString projectKey READ projectKey WRITE setProjectKey NOTIFY projectKeyChanged)
    Q_PROPERTY(QString storageFile READ storageFile WRITE setStorageFile NOTIFY storageFileChanged)
    Q_PROPERTY(int activityLimit READ activityLimit CONSTANT)

public:
    explicit WorkSessionStore(QObject* parent = nullptr);

    QString projectKey() const;
    void setProjectKey(const QString& key);

    QString storageFile() const;
    void setStorageFile(const QString& path);

    int activityLimit() const { return 200; }

    // Active Working Version hint (scoped by user + project). Returns an empty
    // map when there is no valid saved hint.
    Q_INVOKABLE void setActiveVersion(const QString& workspaceId, const QString& version);
    Q_INVOKABLE QVariantMap activeHint() const;
    Q_INVOKABLE void clearActiveVersion();

    // Activity Log. appendActivity returns true only if the record passed the
    // allowlist/sanitization and was stored; a record for an operation that is
    // already listed replaces it. recentActivity returns newest-first.
    Q_INVOKABLE bool appendActivity(const QVariantMap& record);
    Q_INVOKABLE QVariantList recentActivity() const;

    // Reset ONLY the Work session namespace for the current scope (used on
    // corrupt/unsupported local state). Unrelated application settings are left
    // untouched.
    Q_INVOKABLE void resetWorkSession();

signals:
    void projectKeyChanged();
    void storageFileChanged();
    void activeChanged();
    void activityChanged();
    void warning(const QString& message);

private:
    QString scopeHash() const;   // user + hashed project key
    QString group() const;       // "work/<scopeHash>"

    // Returns the sanitized, allowlisted record, or an empty map (and reason via
    // `rejected`) if the record must be rejected.
    static QVariantMap sanitizeActivity(const QVariantMap& record, QString& rejected);

    QString project_key_;
    QString storage_file_;
};

}  // namespace isvn::gui
