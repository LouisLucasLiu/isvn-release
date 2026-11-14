// test_work_session_store — GUI-local session store: Active version
// persistence + bounded, allowlisted Activity Log. All tests use an isolated
// temporary INI file and never touch the real user profile.
#include "backend/WorkSessionStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

using isvn::gui::WorkSessionStore;

class WorkSessionStoreTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir dir_;
    QString file() const { return dir_.filePath(QStringLiteral("work-session.ini")); }

    static QVariantMap record(const QString& id, const QString& target = QStringLiteral("rtl/cpu/RTL/v1")) {
        return QVariantMap{{QStringLiteral("operationId"), id},
                           {QStringLiteral("operationType"), QStringLiteral("checkin")},
                           {QStringLiteral("target"), target},
                           {QStringLiteral("status"), QStringLiteral("success")},
                           {QStringLiteral("summary"), QStringLiteral("did a thing")}};
    }

private slots:
    void saveAndLoadActive();
    void userProjectIsolation();
    void dirtyStateSwitchPersists();
    void activityBoundedTo200();
    void deterministicEviction();
    void allowlistOnly();
    void oneRecordPerOperation();
    void firstWriteCreatesTheSettingsDirectory();
    void rejectsAbsolutePathTarget();
    void rejectsExternalPathInAnyAllowlistedField();
    void writerLockFailsClosedWithoutLosingRecords();
    void corruptSchemaRecovers();
    void resetOnlyWorkNamespace();
    void temporaryIsolationFromRealProfile();
};

void WorkSessionStoreTest::saveAndLoadActive() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/data/projects/proj"));
    QVERIFY(store.activeHint().isEmpty());
    store.setActiveVersion(QStringLiteral("ws_abc"), QStringLiteral("rtl/cpu/RTL/v1"));
    const QVariantMap hint = store.activeHint();
    QCOMPARE(hint.value(QStringLiteral("workspace_id")).toString(), QStringLiteral("ws_abc"));
    QCOMPARE(hint.value(QStringLiteral("version")).toString(), QStringLiteral("rtl/cpu/RTL/v1"));
    store.clearActiveVersion();
    QVERIFY(store.activeHint().isEmpty());
}

void WorkSessionStoreTest::userProjectIsolation() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/data/projects/projA"));
    store.setActiveVersion(QStringLiteral("ws_a"), QStringLiteral("A/x/RTL/v1"));
    // Switching to a different project must not see the other project's hint.
    store.setProjectKey(QStringLiteral("/data/projects/projB"));
    QVERIFY(store.activeHint().isEmpty());
    // Returning restores it.
    store.setProjectKey(QStringLiteral("/data/projects/projA"));
    QCOMPARE(store.activeHint().value(QStringLiteral("version")).toString(),
             QStringLiteral("A/x/RTL/v1"));
}

void WorkSessionStoreTest::dirtyStateSwitchPersists() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-dirty"));
    store.setActiveVersion(QStringLiteral("ws_1"), QStringLiteral("a/b/RTL/v1"));
    store.setActiveVersion(QStringLiteral("ws_2"), QStringLiteral("a/b/SYN/v2"));
    QCOMPARE(store.activeHint().value(QStringLiteral("version")).toString(),
             QStringLiteral("a/b/SYN/v2"));
}

void WorkSessionStoreTest::activityBoundedTo200() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-bound"));
    for (int i = 0; i < 250; ++i) {
        QVERIFY(store.appendActivity(record(QStringLiteral("op%1").arg(i))));
    }
    QCOMPARE(store.recentActivity().size(), 200);
}

void WorkSessionStoreTest::deterministicEviction() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-evict"));
    for (int i = 0; i < 205; ++i) {
        store.appendActivity(record(QStringLiteral("op%1").arg(i)));
    }
    const QVariantList recent = store.recentActivity();
    QCOMPARE(recent.size(), 200);
    // Newest first; the 5 oldest (op0..op4) were evicted.
    QCOMPARE(recent.first().toMap().value(QStringLiteral("operationId")).toString(),
             QStringLiteral("op204"));
    QCOMPARE(recent.last().toMap().value(QStringLiteral("operationId")).toString(),
             QStringLiteral("op5"));
}

// A later record for the same operation replaces the earlier one and moves to
// the front; records of other operations keep their order.
void WorkSessionStoreTest::oneRecordPerOperation() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-upsert"));
    QVERIFY(store.appendActivity(record(QStringLiteral("op-a"))));
    QVERIFY(store.appendActivity(record(QStringLiteral("op-b"))));
    QVariantMap detailed = record(QStringLiteral("op-a"));
    detailed.insert(QStringLiteral("status"), QStringLiteral("committed"));
    detailed.insert(QStringLiteral("itemCount"), QStringLiteral("2"));
    QVERIFY(store.appendActivity(detailed));

    const QVariantList recent = store.recentActivity();
    QCOMPARE(recent.size(), 2);
    const QVariantMap newest = recent.at(0).toMap();
    QCOMPARE(newest.value(QStringLiteral("operationId")).toString(), QStringLiteral("op-a"));
    QCOMPARE(newest.value(QStringLiteral("status")).toString(), QStringLiteral("committed"));
    QCOMPARE(newest.value(QStringLiteral("itemCount")).toString(), QStringLiteral("2"));
    QCOMPARE(recent.at(1).toMap().value(QStringLiteral("operationId")).toString(),
             QStringLiteral("op-b"));
}

// A new account has no settings directory yet; the first write must create it
// instead of failing to take the writer lock.
void WorkSessionStoreTest::firstWriteCreatesTheSettingsDirectory() {
    const QString nested = dir_.filePath(QStringLiteral("new-account/config/isvn/work-session.ini"));
    QVERIFY(!QFileInfo::exists(QFileInfo(nested).absolutePath()));
    WorkSessionStore store;
    store.setStorageFile(nested);
    store.setProjectKey(QStringLiteral("/p-fresh"));
    QVERIFY(store.appendActivity(record(QStringLiteral("op-first"))));
    QCOMPARE(store.recentActivity().size(), 1);
    store.setActiveVersion(QStringLiteral("ws_abc"), QStringLiteral("rtl/cpu/RTL/v1"));
    QCOMPARE(store.activeHint().value(QStringLiteral("version")).toString(),
             QStringLiteral("rtl/cpu/RTL/v1"));
}

void WorkSessionStoreTest::allowlistOnly() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-allow"));
    QVariantMap rec = record(QStringLiteral("op1"));
    rec.insert(QStringLiteral("rawCommand"), QStringLiteral("isvn checkin /secret/path"));
    rec.insert(QStringLiteral("password"), QStringLiteral("hunter2"));
    rec.insert(QStringLiteral("env"), QStringLiteral("PATH=/x"));
    QVERIFY(store.appendActivity(rec));
    const QVariantMap stored = store.recentActivity().first().toMap();
    QVERIFY(!stored.contains(QStringLiteral("rawCommand")));
    QVERIFY(!stored.contains(QStringLiteral("password")));
    QVERIFY(!stored.contains(QStringLiteral("env")));
    QVERIFY(stored.contains(QStringLiteral("operationId")));
    QVERIFY(stored.contains(QStringLiteral("target")));
}

void WorkSessionStoreTest::rejectsAbsolutePathTarget() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-reject"));
    // Absolute/external target is rejected entirely.
    QVERIFY(!store.appendActivity(record(QStringLiteral("op1"), QStringLiteral("/abs/path"))));
    QVERIFY(!store.appendActivity(record(QStringLiteral("op2"),
                                         QStringLiteral("file:///repo"))));
    // Missing required fields are rejected.
    QVERIFY(!store.appendActivity(QVariantMap{{QStringLiteral("target"),
                                               QStringLiteral("a/b/RTL/v1")}}));
    QCOMPARE(store.recentActivity().size(), 0);
}

void WorkSessionStoreTest::rejectsExternalPathInAnyAllowlistedField() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-privacy"));
    QVariantMap rec = record(QStringLiteral("op-private"));
    rec.insert(QStringLiteral("summary"), QStringLiteral("copied /external/unique-secret.v"));
    QVERIFY(store.appendActivity(rec));
    const QVariantMap stored = store.recentActivity().first().toMap();
    QVERIFY(!stored.contains(QStringLiteral("summary")));
    QFile persisted(file());
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    QVERIFY(!QString::fromUtf8(persisted.readAll()).contains(QStringLiteral("/external/unique-secret.v")));
}

void WorkSessionStoreTest::writerLockFailsClosedWithoutLosingRecords() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-writer-lock"));
    QVERIFY(store.appendActivity(record(QStringLiteral("before"))));
    QLockFile held(file() + QStringLiteral(".work-session.lock"));
    held.setStaleLockTime(30000);
    QVERIFY(held.tryLock(0));
    QVERIFY(!store.appendActivity(record(QStringLiteral("blocked"))));
    held.unlock();
    QVERIFY(store.appendActivity(record(QStringLiteral("after"))));
    const QVariantList records = store.recentActivity();
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.first().toMap().value(QStringLiteral("operationId")).toString(),
             QStringLiteral("after"));
    QCOMPARE(records.last().toMap().value(QStringLiteral("operationId")).toString(),
             QStringLiteral("before"));
}

void WorkSessionStoreTest::corruptSchemaRecovers() {
    // Write a corrupt records blob directly, then verify no crash + empty result.
    {
        QSettings settings(file(), QSettings::IniFormat);
        WorkSessionStore probe;
        probe.setStorageFile(file());
        probe.setProjectKey(QStringLiteral("/p-corrupt"));
        // Seed a valid record so the group exists, then corrupt it.
        probe.appendActivity(record(QStringLiteral("op1")));
    }
    {
        QSettings settings(file(), QSettings::IniFormat);
        // Corrupt every schema marker (keys are nested as work/<scope>/schema).
        const QStringList keys = settings.allKeys();
        for (const QString& key : keys) {
            if (key.endsWith(QStringLiteral("/schema"))) {
                settings.setValue(key, QStringLiteral("bogus/v999"));
            }
        }
        settings.sync();
    }
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-corrupt"));
    QCOMPARE(store.recentActivity().size(), 0);  // unsupported schema -> empty
    QVERIFY(store.activeHint().isEmpty());
}

void WorkSessionStoreTest::resetOnlyWorkNamespace() {
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-reset"));
    store.setActiveVersion(QStringLiteral("ws_1"), QStringLiteral("a/b/RTL/v1"));
    store.appendActivity(record(QStringLiteral("op1")));
    // Unrelated setting outside the work group must survive a reset.
    {
        QSettings settings(file(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("unrelated/key"), QStringLiteral("keep-me"));
        settings.sync();
    }
    store.resetWorkSession();
    QVERIFY(store.activeHint().isEmpty());
    QCOMPARE(store.recentActivity().size(), 0);
    QSettings settings(file(), QSettings::IniFormat);
    QCOMPARE(settings.value(QStringLiteral("unrelated/key")).toString(),
             QStringLiteral("keep-me"));
}

void WorkSessionStoreTest::temporaryIsolationFromRealProfile() {
    // The storage file is inside the temporary dir; nothing else is written.
    WorkSessionStore store;
    store.setStorageFile(file());
    store.setProjectKey(QStringLiteral("/p-corrupt"));
    store.setActiveVersion(QStringLiteral("ws_1"), QStringLiteral("a/b/RTL/v1"));
    QVERIFY(QFile::exists(file()));
    QVERIFY(file().startsWith(dir_.path()));
}

QTEST_MAIN(WorkSessionStoreTest)

#include "test_work_session_store.moc"
