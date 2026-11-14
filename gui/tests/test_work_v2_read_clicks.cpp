// test_work_v2_read_clicks — GUI read-binding interaction test. It
// loads the real ProductShell in read mode with an injected fake CLI (returning
// the exact new JSON shapes) and a temporary-file WorkSessionStore, and verifies
// serialized reads, stale-response protection, Active restore/validation, and
// that no mutation command is ever emitted or mock fallback ever used.
#include "backend/WorkSessionStore.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#include <memory>

using isvn::gui::WorkSessionStore;

// Fake single-flight IsvnCli. Emits synchronously by default; when `defer` is
// set, runJson queues the call and the test flushes responses explicitly to
// simulate interleaving.
class FakeReadCli : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    bool defer = false;
    bool failNext = false;
    QString mode = QStringLiteral("multi");  // multi | empty
    QVector<QStringList> calls;

    bool running() const { return false; }

    Q_INVOKABLE void runJson(const QVariant& arguments) {
        QStringList args;
        for (const QVariant& v : arguments.toList()) args.push_back(v.toString());
        calls.push_back(args);
        if (defer) {
            pending_.push_back(args);
            return;
        }
        emitFor(args);
    }

    void flushNext() {
        if (pending_.isEmpty()) return;
        const QStringList args = pending_.takeFirst();
        emitFor(args);
    }

    int pendingCount() const { return pending_.size(); }

    // A command sent by another component through the same bridge failed.
    void emitForeignFailure() { emit commandFailed(QStringLiteral("isvn: another component failed (fake)")); }

signals:
    void runningChanged();
    void commandSucceeded(const QVariant& json);
    void commandFailed(const QString& message);

private:
    void emitFor(const QStringList& args) {
        if (failNext) {
            failNext = false;
            emit commandFailed(QStringLiteral("isvn: backend unavailable (fake)"));
            return;
        }
        emit commandSucceeded(responseFor(args));
    }

    static QVariantMap version(const QString& wid, const QString& ph, const QString& bl,
                               const QString& st, const QString& ver, int ch, int cf, int lo,
                               const QString& health) {
        const QString rp = ph + "/" + bl + "/" + st + "/" + ver;
        return QVariantMap{
            {QStringLiteral("workspace_id"), wid}, {QStringLiteral("version"), ver},
            {QStringLiteral("phase"), ph},         {QStringLiteral("block"), bl},
            {QStringLiteral("stage"), st},         {QStringLiteral("repository_path"), rp},
            {QStringLiteral("subdir"), rp},        {QStringLiteral("working_copy"), "/w/" + rp},
            {QStringLiteral("counts"), QVariantMap{{QStringLiteral("changed"), ch},
                                                   {QStringLiteral("conflict"), cf},
                                                   {QStringLiteral("local"), lo}}},
            {QStringLiteral("health"), health},
            {QStringLiteral("issues"), QVariantList{}}};
    }

    QVariant responseFor(const QStringList& args) const {
        if (args.contains(QStringLiteral("workspaces"))) {
            QVariantList ws;
            if (mode != QStringLiteral("empty")) {
                ws.push_back(version(QStringLiteral("ws_alpha"), QStringLiteral("P2"),
                                     QStringLiteral("cpu"), QStringLiteral("SYN"),
                                     QStringLiteral("v1.7"), 3, 1, 2,
                                     QStringLiteral("needs_attention")));
                ws.push_back(version(QStringLiteral("ws_beta"), QStringLiteral("P2"),
                                     QStringLiteral("cpu"), QStringLiteral("SYN"),
                                     QStringLiteral("v1.6"), 0, 0, 0, QStringLiteral("clean")));
            }
            return QVariantMap{{QStringLiteral("schema_version"), QStringLiteral("workspaces/v1")},
                               {QStringLiteral("project"), QVariantMap{{QStringLiteral("name"),
                                                                        QStringLiteral("chip")}}},
                               {QStringLiteral("workspaces"), ws},
                               {QStringLiteral("issues"), QVariantList{}}};
        }
        // work-status <version> --json
        QString ver;
        for (const QString& a : args)
            if (a.contains(QLatin1Char('/'))) ver = a;
        auto action = [](bool allowed, const QString& code) {
            return QVariantMap{{QStringLiteral("allowed"), allowed},
                               {QStringLiteral("reason_code"), code},
                               {QStringLiteral("reason_params"), QVariantMap{}}};
        };
        QVariantMap entry{
            {QStringLiteral("path"), QStringLiteral("rtl/cpu.sv")},
            {QStringLiteral("kind"), QStringLiteral("file")},
            {QStringLiteral("size_bytes"), 1234},
            {QStringLiteral("svn_state"), QStringLiteral("M")},
            {QStringLiteral("link_state"), QStringLiteral("LOCAL")},
            {QStringLiteral("source_kind"), QStringLiteral("LOCAL")},
            {QStringLiteral("ui_state"), QStringLiteral("modified")},
            {QStringLiteral("counts"), QVariant()},
            {QStringLiteral("actions"), QVariantMap{
                                            {QStringLiteral("make_editable"),
                                             action(false, QStringLiteral("ALREADY_LOCAL"))},
                                            {QStringLiteral("add_to_checkin_batch"),
                                             action(true, QStringLiteral("ALLOWED"))},
                                            {QStringLiteral("revert"),
                                             action(true, QStringLiteral("ALLOWED"))},
                                            {QStringLiteral("remove"),
                                             action(true, QStringLiteral("ALLOWED"))}}},
            {QStringLiteral("issues"), QVariantList{}}};
        return QVariantMap{{QStringLiteral("schema_version"), QStringLiteral("work-status/v1")},
                           {QStringLiteral("workspace_id"), QStringLiteral("ws")},
                           {QStringLiteral("version"), ver},
                           {QStringLiteral("repository_path"), ver},
                           {QStringLiteral("working_copy"), "/w/" + ver},
                           {QStringLiteral("svn_available"), true},
                           {QStringLiteral("summary"), QVariantMap{}},
                           {QStringLiteral("entries"), QVariantList{entry}},
                           {QStringLiteral("version_actions"), QVariantMap{}},
                           {QStringLiteral("issues"), QVariantList{}}};
    }

    QVector<QStringList> pending_;
};

class WorkV2ReadClicksTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir dir_;

    struct Loaded {
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem* root = nullptr;
        QObject* controller = nullptr;
    };

    std::unique_ptr<Loaded> loadReadShell(FakeReadCli* cli, WorkSessionStore* store) {
        auto l = std::make_unique<Loaded>();
        l->engine = std::make_unique<QQmlEngine>();
        l->window = std::make_unique<QQuickWindow>();
        l->engine->addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        QQmlComponent component(l->engine.get(),
                                QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                    QStringLiteral("/qml/shell/ProductShell.qml")));
        QObject* object = component.create();
        if (object == nullptr) qFatal("%s", qPrintable(component.errorString()));
        l->root = qobject_cast<QQuickItem*>(object);
        l->root->setProperty("isvnCli", QVariant::fromValue(static_cast<QObject*>(cli)));
        l->root->setProperty("sessionStore", QVariant::fromValue(static_cast<QObject*>(store)));
        l->root->setProperty("projectPath", QStringLiteral("/tmp/chip"));
        l->root->setProperty("readMode", true);
        l->window->resize(1600, 900);
        l->root->setParentItem(l->window->contentItem());
        l->root->setSize(QSizeF(1600, 900));
        l->window->show();
        if (!QTest::qWaitForWindowExposed(l->window.get()))
            qFatal("window not exposed");
        l->controller = l->root->findChild<QObject*>(QStringLiteral("workReadController"));
        if (l->controller == nullptr) qFatal("workReadController not found");
        return l;
    }

    static void refresh(QObject* controller) {
        QMetaObject::invokeMethod(controller, "refresh");
        QCoreApplication::processEvents();
    }
    static QString state(QObject* c) { return c->property("state").toString(); }
    static QString activeId(QObject* c) { return c->property("activeVersionId").toString(); }
    static int versionCount(QObject* c) { return c->property("localVersions").toList().size(); }

    WorkSessionStore* newStore(const QString& name, const QString& projectKey) {
        auto* store = new WorkSessionStore(this);
        store->setStorageFile(dir_.filePath(name));
        store->setProjectKey(projectKey);
        return store;
    }

private slots:
    void serializedWorkspacesThenWorkStatus();
    void onlyCheckedOutVersionsDisplayed();
    void noSavedActiveMeansNoSelection();
    void validSavedActiveRestored();
    void staleSavedActiveUnselected();
    void staleResponseIgnoredAfterFastSwitch();
    void realReadFailureNeverFallsBackToMock();
    void foreignBridgeFailureDoesNotMarkWorkDataStale();
    void mutationCommandsNeverEmitted();
    void activityLogStoresSafeFieldsOnly();
};

void WorkV2ReadClicksTest::serializedWorkspacesThenWorkStatus() {
    FakeReadCli cli;
    auto* store = newStore(QStringLiteral("s1.ini"), QStringLiteral("/p1"));
    store->setActiveVersion(QStringLiteral("ws_alpha"), QStringLiteral("P2/cpu/SYN/v1.7"));
    auto l = loadReadShell(&cli, store);
    refresh(l->controller);
    // First call is workspaces, second is work-status for the restored Active.
    QVERIFY(cli.calls.size() >= 2);
    QCOMPARE(cli.calls.at(0).first(), QStringLiteral("workspaces"));
    QCOMPARE(cli.calls.at(1).first(), QStringLiteral("work-status"));
    QVERIFY(cli.calls.at(1).contains(QStringLiteral("P2/cpu/SYN/v1.7")));
}

void WorkV2ReadClicksTest::onlyCheckedOutVersionsDisplayed() {
    FakeReadCli cli;
    auto* store = newStore(QStringLiteral("s2.ini"), QStringLiteral("/p2"));
    auto l = loadReadShell(&cli, store);
    refresh(l->controller);
    // The fake returns exactly two workspaces; the model shows exactly those.
    QCOMPARE(versionCount(l->controller), 2);
}

void WorkV2ReadClicksTest::noSavedActiveMeansNoSelection() {
    FakeReadCli cli;
    auto* store = newStore(QStringLiteral("s3.ini"), QStringLiteral("/p3"));
    auto l = loadReadShell(&cli, store);
    refresh(l->controller);
    // No saved hint -> no Active auto-selection, no work-status call.
    QVERIFY(activeId(l->controller).isEmpty());
    for (const QStringList& c : cli.calls)
        QVERIFY(c.first() != QStringLiteral("work-status"));
    QCOMPARE(state(l->controller), QStringLiteral("ready"));
}

void WorkV2ReadClicksTest::validSavedActiveRestored() {
    FakeReadCli cli;
    auto* store = newStore(QStringLiteral("s4.ini"), QStringLiteral("/p4"));
    store->setActiveVersion(QStringLiteral("ws_beta"), QStringLiteral("P2/cpu/SYN/v1.6"));
    auto l = loadReadShell(&cli, store);
    refresh(l->controller);
    QCOMPARE(activeId(l->controller), QStringLiteral("P2/cpu/SYN/v1.6"));
}

void WorkV2ReadClicksTest::staleSavedActiveUnselected() {
    FakeReadCli cli;
    auto* store = newStore(QStringLiteral("s5.ini"), QStringLiteral("/p5"));
    // A saved Active that is NOT in the returned inventory must be cleared.
    store->setActiveVersion(QStringLiteral("ws_gone"), QStringLiteral("P9/gone/RTL/v0"));
    auto l = loadReadShell(&cli, store);
    refresh(l->controller);
    QVERIFY(activeId(l->controller).isEmpty());
    QVERIFY(store->activeHint().isEmpty());  // stale hint cleared safely
}

void WorkV2ReadClicksTest::staleResponseIgnoredAfterFastSwitch() {
    FakeReadCli cli;
    auto* store = newStore(QStringLiteral("s6.ini"), QStringLiteral("/p6"));
    auto l = loadReadShell(&cli, store);
    refresh(l->controller);  // workspaces resolves synchronously; no Active yet
    QObject* c = l->controller;
    cli.defer = true;
    QMetaObject::invokeMethod(c, "selectActive",
                              Q_ARG(QVariant, QVariant(QStringLiteral("P2/cpu/SYN/v1.7"))));
    QMetaObject::invokeMethod(c, "selectActive",
                              Q_ARG(QVariant, QVariant(QStringLiteral("P2/cpu/SYN/v1.6"))));
    QCOMPARE(activeId(c), QStringLiteral("P2/cpu/SYN/v1.6"));
    // Flush the FIRST (stale) work-status response for v1.7; it must be ignored.
    cli.flushNext();
    QCoreApplication::processEvents();
    // Flush the second (current) response for v1.6; it is accepted.
    cli.flushNext();
    QCoreApplication::processEvents();
    const QVariantList files = c->property("activeFiles").toList();
    QVERIFY(!files.isEmpty());
    QCOMPARE(activeId(c), QStringLiteral("P2/cpu/SYN/v1.6"));
}

void WorkV2ReadClicksTest::realReadFailureNeverFallsBackToMock() {
    FakeReadCli cli;
    cli.failNext = true;
    auto* store = newStore(QStringLiteral("s7.ini"), QStringLiteral("/p7"));
    auto l = loadReadShell(&cli, store);
    refresh(l->controller);
    QCOMPARE(state(l->controller), QStringLiteral("error"));
    // No fictional mock versions leaked in on failure.
    QCOMPARE(versionCount(l->controller), 0);
    QVERIFY(!l->controller->property("errorText").toString().isEmpty());
}

void WorkV2ReadClicksTest::foreignBridgeFailureDoesNotMarkWorkDataStale() {
    FakeReadCli cli;
    auto* store = newStore(QStringLiteral("s7b.ini"), QStringLiteral("/p7b"));
    auto l = loadReadShell(&cli, store);
    refresh(l->controller);
    const QString loaded = state(l->controller);
    QVERIFY(loaded != QStringLiteral("error"));

    // Another component's command fails while no read of this controller runs.
    cli.emitForeignFailure();
    QCoreApplication::processEvents();
    QCOMPARE(state(l->controller), loaded);
    QVERIFY(l->controller->property("errorText").toString().isEmpty());

    // The controller's own failed read is still reported.
    cli.failNext = true;
    refresh(l->controller);
    QCOMPARE(state(l->controller), QStringLiteral("error"));
}

void WorkV2ReadClicksTest::mutationCommandsNeverEmitted() {
    FakeReadCli cli;
    auto* store = newStore(QStringLiteral("s8.ini"), QStringLiteral("/p8"));
    store->setActiveVersion(QStringLiteral("ws_alpha"), QStringLiteral("P2/cpu/SYN/v1.7"));
    auto l = loadReadShell(&cli, store);
    refresh(l->controller);
    QMetaObject::invokeMethod(l->controller, "selectActive",
                              Q_ARG(QVariant, QVariant(QStringLiteral("P2/cpu/SYN/v1.6"))));
    QCoreApplication::processEvents();
    static const QStringList kMutations{"checkin", "edit",    "remove", "create",
                                        "checkout", "revert", "import", "delete",
                                        "make-editable"};
    for (const QStringList& c : cli.calls) {
        QVERIFY2(c.first() == QStringLiteral("workspaces") ||
                     c.first() == QStringLiteral("work-status"),
                 qPrintable(c.join(' ')));
        for (const QString& token : c)
            QVERIFY2(!kMutations.contains(token), qPrintable(token));
    }
}

void WorkV2ReadClicksTest::activityLogStoresSafeFieldsOnly() {
    auto* store = newStore(QStringLiteral("s9.ini"), QStringLiteral("/p9"));
    QVariantMap rec{{QStringLiteral("operationId"), QStringLiteral("op1")},
                    {QStringLiteral("operationType"), QStringLiteral("switch-active")},
                    {QStringLiteral("target"), QStringLiteral("P2/cpu/SYN/v1.7")},
                    {QStringLiteral("status"), QStringLiteral("ok")},
                    {QStringLiteral("rawCommand"), QStringLiteral("isvn work-status /abs")}};
    QVERIFY(store->appendActivity(rec));
    const QVariantMap stored = store->recentActivity().first().toMap();
    QVERIFY(stored.contains(QStringLiteral("operationId")));
    QVERIFY(!stored.contains(QStringLiteral("rawCommand")));
}

QTEST_MAIN(WorkV2ReadClicksTest)

#include "test_work_v2_read_clicks.moc"
