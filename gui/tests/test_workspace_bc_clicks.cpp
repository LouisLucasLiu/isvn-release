#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QVariant>

#include <memory>
#include <utility>

class FakeIsvnCli : public QObject {
    Q_OBJECT

public:
    // When true, reconcile returns consistent=null (no copy-from data) instead of false.
    bool nullConsistentReconcile = false;
    // deferred: hold each response until flushNext(), and reject a second command
    // while one is pending, as the real single-command bridge does.
    bool deferred = false;
    bool emptyLog = false;
    QStringList failingCommands;

    Q_INVOKABLE void runJson(const QVariant& arguments) {
        QStringList args;
        for (const QVariant& value : arguments.toList()) {
            args.push_back(value.toString());
        }
        calls.push_back(args);
        if (deferred && !pending.isEmpty()) {
            emit commandFailed(QStringLiteral("another isvn command is running"));
            return;
        }
        if (deferred) {
            pending.push_back(args);
            return;
        }
        respond(args);
    }

    void flushNext() {
        if (pending.isEmpty()) return;
        respond(pending.takeFirst());
    }

    QVector<QStringList> calls;
    QVector<QStringList> pending;

signals:
    void commandSucceeded(QVariant json);
    void commandFailed(QString message);

private:
    void respond(const QStringList& args) {
        if (!args.isEmpty() && failingCommands.contains(args.first())) {
            emit commandFailed(QStringLiteral("simulated failure: ") + args.first());
            return;
        }
        emit commandSucceeded(responseFor(args));
    }

    QVariant responseFor(const QStringList& args) const {
        // Workspace B scopes log/lineage-audit/reconcile to an optional trailing
        // version arg; match on the command tokens so a version arg is tolerated.
        if (emptyLog && !args.isEmpty() && args.first() == QStringLiteral("log"))
            return QVariantMap{{QStringLiteral("entries"), QVariantList{}}};
        if (!args.isEmpty() && args.first() == QStringLiteral("log")) {
            QVariantMap entry;
            entry.insert(QStringLiteral("rev"), 42);
            entry.insert(QStringLiteral("author"), QStringLiteral("alice"));
            entry.insert(QStringLiteral("time"), QStringLiteral("2026-06-14T01:00:00Z"));
            entry.insert(QStringLiteral("message"), QStringLiteral("commit message"));
            return QVariantMap{{QStringLiteral("entries"), QVariantList{entry}}};
        }
        if (args.size() >= 2 && args.first() == QStringLiteral("lineage")
            && args.at(1) == QStringLiteral("audit")) {
            QVariantMap create;
            create.insert(QStringLiteral("seq"), 1);
            create.insert(QStringLiteral("ts"), QStringLiteral("2026-06-14T01:01:00Z"));
            create.insert(QStringLiteral("actor"), QStringLiteral("alice"));
            create.insert(QStringLiteral("action"), QStringLiteral("create"));
            create.insert(QStringLiteral("child"), QStringLiteral("Trail/Block1/RTL/v1"));
            create.insert(QStringLiteral("old_parent"), QString());
            create.insert(QStringLiteral("new_parent"), QStringLiteral("ROOT"));
            create.insert(QStringLiteral("reason"), QStringLiteral("create"));
            QVariantMap resolve = create;
            resolve.insert(QStringLiteral("seq"), 2);
            resolve.insert(QStringLiteral("action"), QStringLiteral("reconcile_resolve"));
            resolve.insert(QStringLiteral("old_parent"), QStringLiteral("ROOT"));
            resolve.insert(QStringLiteral("new_parent"), QStringLiteral("Trail/Block1/RTL/v0"));
            QVariantMap remove = create;
            remove.insert(QStringLiteral("seq"), 3);
            remove.insert(QStringLiteral("action"), QStringLiteral("remove"));
            return QVariantMap{{QStringLiteral("records"), QVariantList{create, resolve, remove}}};
        }
        if (!args.isEmpty() && args.first() == QStringLiteral("reconcile")) {
            QVariantMap verify;
            verify.insert(QStringLiteral("intact"), true);
            // When nullConsistentReconcile is set, omit "consistent" to simulate null
            // (the backend returns null when copy-from comparison was not performed).
            QVariantMap result{{QStringLiteral("child"), QStringLiteral("Trail/Block1/RTL/v1")},
                               {QStringLiteral("lineage_parent"), QStringLiteral("ROOT")},
                               {QStringLiteral("svn_copyfrom_hint"), QString()},
                               {QStringLiteral("verify"), verify}};
            if (!nullConsistentReconcile)
                result.insert(QStringLiteral("consistent"), false);
            return result;
        }
        if (args.size() >= 2 && args[0] == QStringLiteral("provenance") &&
            args[1] == QStringLiteral("query")) {
            if (args.contains(QStringLiteral("P1/TOP/RTL/rooted"))) {
                QVariantMap subject{{QStringLiteral("canonical"), QStringLiteral("isvn://chip1/P1/TOP/RTL/rooted")},
                                    {QStringLiteral("phase"), QStringLiteral("P1")},
                                    {QStringLiteral("block"), QStringLiteral("TOP")},
                                    {QStringLiteral("stage"), QStringLiteral("RTL")},
                                    {QStringLiteral("version"), QStringLiteral("rooted")},
                                    {QStringLiteral("resolved"), true}};
                QVariantMap rootNode{{QStringLiteral("canonical"), QStringLiteral("isvn://chip1/ROOT")},
                                     {QStringLiteral("phase"), QVariant()},
                                     {QStringLiteral("block"), QVariant()},
                                     {QStringLiteral("stage"), QVariant()},
                                     {QStringLiteral("version"), QStringLiteral("ROOT")},
                                     {QStringLiteral("depth"), 1},
                                     {QStringLiteral("relation"), QStringLiteral("root")}};
                return QVariantMap{{QStringLiteral("schema_version"), QStringLiteral("isvn.provenance.query.v0")},
                                   {QStringLiteral("authority"), QStringLiteral("declared_provenance")},
                                   {QStringLiteral("content_verified"), false},
                                   {QStringLiteral("status"), QStringLiteral("ok")},
                                   {QStringLiteral("subject"), subject},
                                   {QStringLiteral("ancestor_chain"), QVariantList{rootNode}},
                                   {QStringLiteral("root"), rootNode},
                                   {QStringLiteral("truncated"), false},
                                   {QStringLiteral("warnings"), QVariantList{}}};
            }
            QVariantMap subject{{QStringLiteral("canonical"), QStringLiteral("isvn://chip1/P1/TOP/SYN/v3")},
                                {QStringLiteral("phase"), QStringLiteral("P1")},
                                {QStringLiteral("block"), QStringLiteral("TOP")},
                                {QStringLiteral("stage"), QStringLiteral("SYN")},
                                {QStringLiteral("version"), QStringLiteral("v3")},
                                {QStringLiteral("resolved"), true}};
            QVariantMap v2{{QStringLiteral("canonical"), QStringLiteral("isvn://chip1/P1/TOP/SYN/v2")},
                           {QStringLiteral("phase"), QStringLiteral("P1")},
                           {QStringLiteral("block"), QStringLiteral("TOP")},
                           {QStringLiteral("stage"), QStringLiteral("SYN")},
                           {QStringLiteral("version"), QStringLiteral("v2")},
                           {QStringLiteral("depth"), 1},
                           {QStringLiteral("relation"), QStringLiteral("parent")}};
            QVariantMap v1 = v2;
            v1.insert(QStringLiteral("canonical"), QStringLiteral("isvn://chip1/P1/TOP/SYN/v1"));
            v1.insert(QStringLiteral("version"), QStringLiteral("v1"));
            v1.insert(QStringLiteral("depth"), 2);
            v1.insert(QStringLiteral("relation"), QStringLiteral("ancestor"));
            QVariantMap base = v2;
            base.insert(QStringLiteral("canonical"), QStringLiteral("isvn://chip1/P1/TOP/RTL/base"));
            base.insert(QStringLiteral("stage"), QStringLiteral("RTL"));
            base.insert(QStringLiteral("version"), QStringLiteral("base"));
            base.insert(QStringLiteral("depth"), 3);
            base.insert(QStringLiteral("relation"), QStringLiteral("root"));
            return QVariantMap{{QStringLiteral("schema_version"), QStringLiteral("isvn.provenance.query.v0")},
                               {QStringLiteral("authority"), QStringLiteral("declared_provenance")},
                               {QStringLiteral("content_verified"), false},
                               {QStringLiteral("status"), QStringLiteral("ok")},
                               {QStringLiteral("subject"), subject},
                               {QStringLiteral("ancestor_chain"), QVariantList{v2, v1, base}},
                               {QStringLiteral("root"), base},
                               {QStringLiteral("truncated"), false},
                               {QStringLiteral("warnings"), QVariantList{}}};
        }
        if (args.size() >= 2 && args[0] == QStringLiteral("file") &&
            args[1] == QStringLiteral("query")) {
            QVariantMap file{{QStringLiteral("path"), QStringLiteral("src/top.v")},
                             {QStringLiteral("managed"), true}};
            QVariantMap subject{{QStringLiteral("canonical"), QStringLiteral("isvn://chip1/P1/TOP/SYN/v3")},
                                {QStringLiteral("phase"), QStringLiteral("P1")},
                                {QStringLiteral("block"), QStringLiteral("TOP")},
                                {QStringLiteral("stage"), QStringLiteral("SYN")},
                                {QStringLiteral("version"), QStringLiteral("v3")},
                                {QStringLiteral("file_state"), QStringLiteral("retained")}};
            QVariantMap p1{{QStringLiteral("canonical"), QStringLiteral("isvn://chip1/P1/TOP/SYN/v1")},
                           {QStringLiteral("phase"), QStringLiteral("P1")},
                           {QStringLiteral("block"), QStringLiteral("TOP")},
                           {QStringLiteral("stage"), QStringLiteral("SYN")},
                           {QStringLiteral("version"), QStringLiteral("v1")},
                           {QStringLiteral("depth"), 2},
                           {QStringLiteral("relation"), QStringLiteral("file_parent")}};
            QVariantMap p2 = p1;
            p2.insert(QStringLiteral("canonical"), QStringLiteral("isvn://chip1/P1/TOP/SYN/v2"));
            p2.insert(QStringLiteral("version"), QStringLiteral("v2"));
            p2.insert(QStringLiteral("depth"), 1);
            return QVariantMap{{QStringLiteral("schema_version"), QStringLiteral("isvn.file.query.v0")},
                               {QStringLiteral("authority"), QStringLiteral("declared_inventory")},
                               {QStringLiteral("content_verified"), false},
                               {QStringLiteral("status"), QStringLiteral("ok")},
                               {QStringLiteral("file"), file},
                               {QStringLiteral("subjects"), QVariantList{subject}},
                               {QStringLiteral("parent_versions"), QVariantList{p1, p2}},
                               {QStringLiteral("root_versions"), QVariantList{}},
                               {QStringLiteral("warnings"), QVariantList{}}};
        }
        if (args == QStringList{"pool-stat"}) {
            QVariantMap dist;
            dist.insert(QStringLiteral("1"), 2);
            QVariantMap renew;
            renew.insert(QStringLiteral("hash"), QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
            renew.insert(QStringLiteral("actor"), QStringLiteral("alice"));
            renew.insert(QStringLiteral("renewed_at"), QStringLiteral("2026-06-14T01:02:00Z"));
            renew.insert(QStringLiteral("refcount"), 2);
            renew.insert(QStringLiteral("size"), 99);
            return QVariantMap{{QStringLiteral("dedup_ratio"), 1.5},
                               {QStringLiteral("used_bytes"), 10},
                               {QStringLiteral("capacity_bytes"), 100},
                               {QStringLiteral("object_count"), 2},
                               {QStringLiteral("ref_count"), 3},
                               {QStringLiteral("physical_bytes"), 9},
                               {QStringLiteral("logical_ref_bytes"), 14},
                               {QStringLiteral("atime_usable"), true},
                               {QStringLiteral("renewed_object_count"), 1},
                               {QStringLiteral("renewed_bytes"), 99},
                               {QStringLiteral("recent_renews"), QVariantList{renew}},
                               {QStringLiteral("refcount_dist"), dist}};
        }
        if (args.size() >= 2 && args[0] == QStringLiteral("pool-stat") &&
            args[1] == QStringLiteral("--evict-events")) {
            QVariantMap event;
            event.insert(QStringLiteral("hash"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
            event.insert(QStringLiteral("evicted_at"), QStringLiteral("2026-06-14T01:03:00Z"));
            QVariantMap density;
            density.insert(QStringLiteral("window"), QStringLiteral("all"));
            density.insert(QStringLiteral("count"), 1);
            QVariantMap result = responseFor(QStringList{"pool-stat"}).toMap();
            result.insert(QStringLiteral("events"), QVariantList{event});
            result.insert(QStringLiteral("density"), QVariantList{density});
            result.insert(QStringLiteral("threshold"), 10);
            result.insert(QStringLiteral("critical"), false);
            return result;
        }
        if (args == QStringList{"gc", "--reconcile", "--dry-run"}) {
            QVariantMap phantom;
            phantom.insert(QStringLiteral("hash"), QStringLiteral("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"));
            phantom.insert(QStringLiteral("link_path"), QStringLiteral("/tmp/work/file.gds"));
            return QVariantMap{{QStringLiteral("dry_run"), true},
                               {QStringLiteral("phantom_refs"), QVariantList{phantom}}};
        }
        if (args == QStringList{"gc", "--reconcile"} || args == QStringList{"gc"} ||
            (args.size() >= 2 && args[0] == QStringLiteral("gc") &&
             args[1] == QStringLiteral("--reclaim"))) {
            return QVariantMap{{QStringLiteral("dry_run"), false},
                               {QStringLiteral("refs_removed"), 1}};
        }
        if (args == QStringList{"lineage", "verify"}) {
            QVariantMap detail;
            detail.insert(QStringLiteral("message"), QStringLiteral("ok"));
            return QVariantMap{{QStringLiteral("intact"), true},
                               {QStringLiteral("break_at"), QVariant()},
                               {QStringLiteral("detail"), detail}};
        }
        if (args == QStringList{"admin", "status"}) {
            QVariantMap path;
            path.insert(QStringLiteral("name"), QStringLiteral("pool"));
            path.insert(QStringLiteral("path"), QStringLiteral("/tmp/pool"));
            path.insert(QStringLiteral("exists"), true);
            path.insert(QStringLiteral("reachable"), true);
            path.insert(QStringLiteral("available_bytes"), 123);
            path.insert(QStringLiteral("mount_root"), QStringLiteral("/tmp"));
            QVariantMap permission;
            permission.insert(QStringLiteral("name"), QStringLiteral("pool_objects"));
            permission.insert(QStringLiteral("path"), QStringLiteral("/tmp/pool/objects"));
            permission.insert(QStringLiteral("mode"), QStringLiteral("3770"));
            permission.insert(QStringLiteral("expected_mode"), QStringLiteral("3770"));
            permission.insert(QStringLiteral("group"), QStringLiteral("proj"));
            permission.insert(QStringLiteral("matches_expected"), true);
            QVariantMap scale;
            scale.insert(QStringLiteral("phase_count"), 1);
            scale.insert(QStringLiteral("block_count"), 1);
            scale.insert(QStringLiteral("stage_count"), 1);
            scale.insert(QStringLiteral("version_count"), 1);
            scale.insert(QStringLiteral("member_count"), 1);
            scale.insert(QStringLiteral("revision_count"), 1);
            QVariantMap lineage;
            lineage.insert(QStringLiteral("intact"), true);
            QVariantMap backup;
            backup.insert(QStringLiteral("available"), true);
            backup.insert(QStringLiteral("latest"), QStringLiteral("2026-06-14T01:04:00Z"));
            return QVariantMap{{QStringLiteral("paths"), QVariantList{path}},
                               {QStringLiteral("permissions"), QVariantList{permission}},
                               {QStringLiteral("authz"), QVariantMap{}},
                               {QStringLiteral("scale"), scale},
                               {QStringLiteral("lineage"), lineage},
                               {QStringLiteral("backup"), backup}};
        }
        if (args == QStringList{"branches"}) return QVariantMap{{QStringLiteral("branches"), QVariantList{}}};
        if (args == QStringList{"status"}) return QVariantMap{{QStringLiteral("version"), QString()}, {QStringLiteral("files"), QVariantList{}}};
        return QVariantMap{};
    }
};

class FakeRoleResolver : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString roleName READ roleName NOTIFY roleChanged)
    Q_PROPERTY(bool allowed READ allowed NOTIFY roleChanged)
    Q_PROPERTY(QString message READ message NOTIFY roleChanged)
    Q_PROPERTY(QString currentUser READ currentUser NOTIFY roleChanged)

public:
    explicit FakeRoleResolver(QString role, QObject* parent = nullptr)
        : QObject(parent), role_(std::move(role)) {}

    QString roleName() const { return role_; }
    bool allowed() const { return true; }
    QString message() const { return role_; }
    QString currentUser() const { return QStringLiteral("tester"); }

    Q_INVOKABLE QVariantMap refresh(const QString&) {
        emit roleChanged();
        return QVariantMap{{QStringLiteral("role"), role_}, {QStringLiteral("allowed"), true}};
    }

signals:
    void roleChanged();

private:
    QString role_;
};

class WorkspaceBCClicksTest : public QObject {
    Q_OBJECT

private slots:
    void mainLanguageSwitching();
    void workspaceAParentOptionFollowsLanguage();
    void workspaceBButtonsRenderSeparatePanels();
    void workspaceBSelectionLoadsNoEvidenceUntilRefresh();
    void workspaceASelectionShowsGraphOnlyOnRefresh();
    void workspaceBLateResponseForPreviousVersionIsDiscarded();
    void workspaceBRefreshWhileRunningDoesNotCutItShort();
    void workspaceBFailedRefreshIsShownAsAnError();
    void workspaceBEmptyEvidenceIsNotAnError();
    void workspaceCButtonsRenderStorageOnlyPanels();
    void mainRoleGatingForWorkspaceC();
    void sourceCheckNullConsistentNoHintIsNotRun();
    void workspaceBLineageExplorerQueriesAreReadOnly();
    void workspaceBLineageGraphRendersRootSentinel();

private:
    struct LoadedItem {
        std::unique_ptr<QQmlEngine> engine;
        FakeIsvnCli cli;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem* root = nullptr;
    };

    static QQuickItem* findItem(QObject* root, const char* name) {
        auto* item = root->findChild<QQuickItem*>(QString::fromLatin1(name));
        if (item == nullptr) qFatal("missing QML item: %s", name);
        return item;
    }

    static void click(QQuickWindow& window, QQuickItem* item) {
        QTRY_VERIFY(item->width() > 0);
        QTRY_VERIFY(item->height() > 0);
        QVERIFY(item->isEnabled());
        if (QMetaObject::invokeMethod(item, "clicked")) {
            QCoreApplication::processEvents();
            return;
        }
        const QPointF point = item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0));
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point.toPoint());
        QCoreApplication::processEvents();
    }

    static void mouseClick(QQuickWindow& window, QQuickItem* item) {
        QTRY_VERIFY(item->width() > 0);
        QTRY_VERIFY(item->height() > 0);
        QVERIFY(item->isEnabled());
        const QPointF point = item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0));
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point.toPoint());
        QCoreApplication::processEvents();
    }

    std::unique_ptr<LoadedItem> loadItem(const QString& relative_path, int width = 1280,
                                         int height = 1100) {
        auto loaded = std::make_unique<LoadedItem>();
        loaded->engine = std::make_unique<QQmlEngine>();
        loaded->window = std::make_unique<QQuickWindow>();
        loaded->engine->rootContext()->setContextProperty(QStringLiteral("isvnCli"), &loaded->cli);
        loaded->engine->addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        QQmlComponent component(loaded->engine.get(),
                                QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                    QStringLiteral("/") + relative_path));
        QObject* object = component.create();
        if (object == nullptr) qFatal("%s", qPrintable(component.errorString()));
        loaded->root = qobject_cast<QQuickItem*>(object);
        if (loaded->root == nullptr) qFatal("loaded QML root is not a QQuickItem");
        loaded->window->resize(width, height);
        loaded->root->setParentItem(loaded->window->contentItem());
        loaded->root->setSize(QSizeF(width, height));
        loaded->window->show();
        if (!QTest::qWaitForWindowExposed(loaded->window.get())) {
            qFatal("QQuickWindow was not exposed");
        }
        QCoreApplication::processEvents();
        return loaded;
    }

    std::unique_ptr<QQmlApplicationEngine> loadMain(FakeIsvnCli& cli, FakeRoleResolver& role) {
        auto engine = std::make_unique<QQmlApplicationEngine>();
        engine->rootContext()->setContextProperty(QStringLiteral("isvnCli"), &cli);
        engine->rootContext()->setContextProperty(QStringLiteral("roleResolver"), &role);
        engine->addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        engine->load(QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                         QStringLiteral("/qml/Main.qml")));
        if (engine->rootObjects().isEmpty()) qFatal("unable to load Main.qml");
        QCoreApplication::processEvents();
        return engine;
    }
};

void WorkspaceBCClicksTest::workspaceAParentOptionFollowsLanguage() {
    // The create-parent ComboBox's "use focused version" entry is the only
    // translatable dropdown option. It must follow language changes rather than
    // freeze at the language present when the model was populated.
    FakeIsvnCli cli;
    FakeRoleResolver role(QStringLiteral("PM"));
    auto engine = loadMain(cli, role);
    auto* root = qobject_cast<QQuickWindow*>(engine->rootObjects().first());
    QVERIFY(root != nullptr);
    auto* i18n = root->findChild<QObject*>(QStringLiteral("i18n"));
    QVERIFY(i18n != nullptr);

    auto* combo = root->findChild<QQuickItem*>(QStringLiteral("createParentCombo"));
    QVERIFY(combo != nullptr);
    // populateBranches always appends the focus option first; wait for it.
    QTRY_COMPARE(combo->property("currentIndex").toInt(), 0);

    QCOMPARE(combo->property("displayText").toString(),
             QStringLiteral("以目前版本為 parent"));

    i18n->setProperty("language", QStringLiteral("en"));
    QCoreApplication::processEvents();
    QCOMPARE(combo->property("displayText").toString(),
             QStringLiteral("Use selected version as parent"));

    i18n->setProperty("language", QStringLiteral("zh_CN"));
    QCoreApplication::processEvents();
    QCOMPARE(combo->property("displayText").toString(),
             QStringLiteral("以当前版本为 parent"));
}

void WorkspaceBCClicksTest::workspaceBButtonsRenderSeparatePanels() {
    auto loaded = loadItem(QStringLiteral("qml/workspaceB/WorkspaceB.qml"));
    // Adopt the app-shell selected version so the evidence panels/actions show.
    loaded->root->setProperty("shellVersion", QStringLiteral("Trail/Block1/RTL/v1"));

    // One Refresh evidence action loads SVN commit history, provenance log, and the
    // source check into their separate models (no unified history model).
    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 1);
    QCOMPARE(loaded->root->property("lineageAuditCount").toInt(), 3);
    QCOMPARE(loaded->root->property("lineagePanoramaCount").toInt(), 1);
    QCOMPARE(loaded->cli.calls.last().first(), QStringLiteral("reconcile"));
    QVERIFY(loaded->root->property("reconcileChild").toString().contains(QStringLiteral("Trail")));
    // consistent=false -> user-facing WARNING state (report-only, no mutation).
    QCOMPARE(loaded->root->property("reconcileConsistent").toBool(), false);

    // Source check stays report-only.
    click(*loaded->window, findItem(loaded->root, "runReconcileButton"));
    QCOMPARE(loaded->cli.calls.last().first(), QStringLiteral("reconcile"));

    // Decide source is review-only: it must not issue any new backend call.
    auto* decision = findItem(loaded->root, "reconcileDecisionButton");
    QVERIFY(decision->property("text").toString().contains(QStringLiteral("檢視來源決策")));
    const int calls_before = loaded->cli.calls.size();
    click(*loaded->window, decision);
    QCOMPARE(loaded->cli.calls.size(), calls_before);
}

void WorkspaceBCClicksTest::workspaceBSelectionLoadsNoEvidenceUntilRefresh() {
    auto loaded = loadItem(QStringLiteral("qml/workspaceB/WorkspaceB.qml"));
    const auto evidenceCalls = [&loaded]() {
        int n = 0;
        for (const auto& call : loaded->cli.calls) {
            const QString cmd = call.isEmpty() ? QString() : call.first();
            if (cmd == QStringLiteral("log") || cmd == QStringLiteral("reconcile") ||
                (cmd == QStringLiteral("lineage") && call.value(1) == QStringLiteral("audit")))
                ++n;
        }
        return n;
    };
    QCOMPARE(evidenceCalls(), 0);

    // Selecting a version (from the shell or the picker) runs no evidence command.
    loaded->root->setProperty("shellVersion", QStringLiteral("Trail/Block1/RTL/v1"));
    QCoreApplication::processEvents();
    QCOMPARE(evidenceCalls(), 0);
    loaded->root->setProperty("versionFilter", QStringLiteral("Trail/Block1/RTL/v1"));
    QCoreApplication::processEvents();
    QCOMPARE(evidenceCalls(), 0);
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 0);

    // Refresh evidence loads log, provenance log and source check once.
    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QCOMPARE(evidenceCalls(), 3);
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 1);
    QCOMPARE(loaded->root->property("lineageAuditCount").toInt(), 3);
    QCOMPARE(loaded->root->property("sourceCheckPhase").toString(), QStringLiteral("ran"));

    // A different version clears the previous version's evidence and loads nothing.
    loaded->root->setProperty("versionFilter", QStringLiteral("Trail/Block1/SYN/v2"));
    QCoreApplication::processEvents();
    QCOMPARE(evidenceCalls(), 3);
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 0);
    QCOMPARE(loaded->root->property("lineageAuditCount").toInt(), 0);
    QCOMPARE(loaded->root->property("lineagePanoramaCount").toInt(), 0);
    QCOMPARE(loaded->root->property("sourceCheckPhase").toString(), QStringLiteral("not_run"));

    // Re-selecting the version whose evidence is shown keeps it.
    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QCOMPARE(evidenceCalls(), 6);
    loaded->root->setProperty("shellVersion", QStringLiteral("Other/Block/RTL/v9"));
    QCoreApplication::processEvents();
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 1);
    QCOMPARE(evidenceCalls(), 6);
}

void WorkspaceBCClicksTest::workspaceASelectionShowsGraphOnlyOnRefresh() {
    auto loaded = loadItem(QStringLiteral("qml/workspaceA/WorkspaceA.qml"));
    const auto count = [&loaded](const QString& command) {
        int n = 0;
        for (const auto& call : loaded->cli.calls)
            if (!call.isEmpty() && call.first() == command) ++n;
        return n;
    };
    QVERIFY(QMetaObject::invokeMethod(loaded->root, "focusVersion",
                                      Q_ARG(QVariant, QStringLiteral("Trail/Block1/RTL/v1"))));
    QCoreApplication::processEvents();
    QVERIFY(count(QStringLiteral("status")) >= 1);
    QCOMPARE(count(QStringLiteral("ancestry")), 0);

    QVERIFY(QMetaObject::invokeMethod(loaded->root, "refreshGraph"));
    QCoreApplication::processEvents();
    QCOMPARE(count(QStringLiteral("ancestry")), 1);
}

namespace {
int evidenceCallCount(const QVector<QStringList>& calls) {
    int n = 0;
    for (const auto& call : calls) {
        const QString cmd = call.isEmpty() ? QString() : call.first();
        if (cmd == QStringLiteral("log") || cmd == QStringLiteral("reconcile") ||
            (cmd == QStringLiteral("lineage") && call.value(1) == QStringLiteral("audit")))
            ++n;
    }
    return n;
}
}  // namespace

void WorkspaceBCClicksTest::workspaceBLateResponseForPreviousVersionIsDiscarded() {
    auto loaded = loadItem(QStringLiteral("qml/workspaceB/WorkspaceB.qml"));
    loaded->cli.deferred = true;
    loaded->root->setProperty("versionFilter", QStringLiteral("Trail/Block1/RTL/v1"));
    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QCOMPARE(loaded->cli.pending.size(), 1);

    // The selection moves on before the first response arrives.
    loaded->root->setProperty("versionFilter", QStringLiteral("Trail/Block1/SYN/v2"));
    QCoreApplication::processEvents();
    loaded->cli.flushNext();
    QCoreApplication::processEvents();
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 0);
    QCOMPARE(loaded->root->property("lineageAuditCount").toInt(), 0);
    QCOMPARE(evidenceCallCount(loaded->cli.calls), 1);
    QCOMPARE(loaded->root->property("evidenceError").toString(), QString());

    // A refresh for the new version loads it normally.
    loaded->cli.deferred = false;
    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 1);
    QCOMPARE(loaded->root->property("lineageAuditCount").toInt(), 3);
    QCOMPARE(loaded->cli.calls.last().value(1), QStringLiteral("Trail/Block1/SYN/v2"));
}

void WorkspaceBCClicksTest::workspaceBRefreshWhileRunningDoesNotCutItShort() {
    auto loaded = loadItem(QStringLiteral("qml/workspaceB/WorkspaceB.qml"));
    loaded->cli.deferred = true;
    loaded->root->setProperty("versionFilter", QStringLiteral("Trail/Block1/RTL/v1"));
    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QCOMPARE(evidenceCallCount(loaded->cli.calls), 1);

    // A second refresh is not offered or sent while one runs.
    QVERIFY(!findItem(loaded->root, "refreshEvidenceButton")->isEnabled());
    QVERIFY(!findItem(loaded->root, "runReconcileButton")->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(loaded->root, "refreshAllReview"));
    QCOMPARE(evidenceCallCount(loaded->cli.calls), 1);
    QVERIFY(findItem(loaded->root, "evidenceStateBanner")->isVisible());

    // The running refresh completes all three steps.
    for (int step = 0; step < 3; ++step) {
        loaded->cli.flushNext();
        QCoreApplication::processEvents();
    }
    QCOMPARE(evidenceCallCount(loaded->cli.calls), 3);
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 1);
    QCOMPARE(loaded->root->property("lineageAuditCount").toInt(), 3);
    QCOMPARE(loaded->root->property("sourceCheckPhase").toString(), QStringLiteral("ran"));
    QCOMPARE(loaded->root->property("refreshAllInProgress").toBool(), false);
    QVERIFY(!findItem(loaded->root, "evidenceStateBanner")->isVisible());
}

void WorkspaceBCClicksTest::workspaceBFailedRefreshIsShownAsAnError() {
    auto loaded = loadItem(QStringLiteral("qml/workspaceB/WorkspaceB.qml"));
    loaded->cli.failingCommands = {QStringLiteral("log")};
    loaded->root->setProperty("versionFilter", QStringLiteral("Trail/Block1/RTL/v1"));
    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 0);
    QVERIFY(!loaded->root->property("evidenceError").toString().isEmpty());
    auto* banner = findItem(loaded->root, "evidenceStateBanner");
    QVERIFY(banner->isVisible());
    QCOMPARE(banner->property("severity").toString(), QStringLiteral("danger"));

    // A later successful refresh clears the error.
    loaded->cli.failingCommands.clear();
    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QCOMPARE(loaded->root->property("evidenceError").toString(), QString());
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 1);
    QVERIFY(!banner->isVisible());

    // An error does not carry over to another version.
    loaded->cli.failingCommands = {QStringLiteral("lineage")};
    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QVERIFY(!loaded->root->property("evidenceError").toString().isEmpty());
    loaded->root->setProperty("versionFilter", QStringLiteral("Trail/Block1/SYN/v2"));
    QCoreApplication::processEvents();
    QCOMPARE(loaded->root->property("evidenceError").toString(), QString());
}

void WorkspaceBCClicksTest::workspaceBEmptyEvidenceIsNotAnError() {
    auto loaded = loadItem(QStringLiteral("qml/workspaceB/WorkspaceB.qml"));
    loaded->cli.emptyLog = true;
    loaded->root->setProperty("versionFilter", QStringLiteral("Trail/Block1/RTL/v1"));
    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QCOMPARE(loaded->root->property("svnLogCount").toInt(), 0);
    QCOMPARE(loaded->root->property("lineageAuditCount").toInt(), 3);
    QCOMPARE(loaded->root->property("evidenceError").toString(), QString());
    QVERIFY(!findItem(loaded->root, "evidenceStateBanner")->isVisible());
}

void WorkspaceBCClicksTest::mainLanguageSwitching() {
    FakeIsvnCli cli;
    FakeRoleResolver role(QStringLiteral("PM"));
    auto engine = loadMain(cli, role);
    auto* root = qobject_cast<QQuickWindow*>(engine->rootObjects().first());
    QVERIFY(root != nullptr);
    auto* i18n = root->findChild<QObject*>(QStringLiteral("i18n"));
    QVERIFY(i18n != nullptr);
    auto* a_tab = root->findChild<QQuickItem*>(QStringLiteral("workspaceATab"));
    auto* b_tab = root->findChild<QQuickItem*>(QStringLiteral("workspaceBTab"));
    auto* c_tab = root->findChild<QQuickItem*>(QStringLiteral("workspaceCTab"));
    QVERIFY(a_tab != nullptr);
    QVERIFY(b_tab != nullptr);
    QVERIFY(c_tab != nullptr);

    QCOMPARE(a_tab->property("text").toString(), QStringLiteral("工作區"));
    QCOMPARE(b_tab->property("text").toString(), QStringLiteral("歷史"));
    QCOMPARE(c_tab->property("text").toString(), QStringLiteral("維護"));

    i18n->setProperty("language", QStringLiteral("en"));
    QCoreApplication::processEvents();
    QCOMPARE(a_tab->property("text").toString(), QStringLiteral("Workspace"));
    QCOMPARE(b_tab->property("text").toString(), QStringLiteral("History"));
    QCOMPARE(c_tab->property("text").toString(), QStringLiteral("Maintenance"));

    i18n->setProperty("language", QStringLiteral("zh_CN"));
    QCoreApplication::processEvents();
    QCOMPARE(a_tab->property("text").toString(), QStringLiteral("工作区"));
    QCOMPARE(b_tab->property("text").toString(), QStringLiteral("历史"));
    QCOMPARE(c_tab->property("text").toString(), QStringLiteral("维护"));
}

void WorkspaceBCClicksTest::workspaceCButtonsRenderStorageOnlyPanels() {
    auto loaded = loadItem(QStringLiteral("qml/workspaceC/WorkspaceC.qml"), 1400, 1500);
    loaded->root->setProperty("pmMode", true);
    QCoreApplication::processEvents();

    click(*loaded->window, findItem(loaded->root, "loadPoolStatButton"));
    QVERIFY2(loaded->cli.calls.last() == QStringList{"pool-stat"},
             qPrintable(QStringLiteral("actual args: ") + loaded->cli.calls.last().join(QStringLiteral(" "))));
    QCOMPARE(loaded->root->property("refcountDistCount").toInt(), 1);
    QCOMPARE(loaded->root->property("renewEventCount").toInt(), 1);
    QCOMPARE(loaded->root->property("renewedObjectCount").toString(), QStringLiteral("1"));

    click(*loaded->window, findItem(loaded->root, "loadSwapHealthButton"));
    QCOMPARE(loaded->cli.calls.last(),
             (QStringList{"pool-stat", "--evict-events", "--evict-threshold", "10"}));
    QCOMPARE(loaded->root->property("evictEventCount").toInt(), 1);
    QCOMPARE(loaded->root->property("densityCount").toInt(), 1);

    click(*loaded->window, findItem(loaded->root, "gcPreviewButton"));
    QCOMPARE(loaded->cli.calls.last(), (QStringList{"gc", "--reconcile", "--dry-run"}));
    QVERIFY(loaded->root->property("previewReady").toBool());
    QCOMPARE(loaded->root->property("phantomCount").toInt(), 1);

    const int calls_before_reconcile = loaded->cli.calls.size();
    click(*loaded->window, findItem(loaded->root, "gcReconcileButton"));
    QCOMPARE(loaded->root->property("pendingGcArgs").toStringList(),
             (QStringList{"gc", "--reconcile"}));
    auto* dialog = loaded->root->findChild<QObject*>(QStringLiteral("gcConfirmDialog"));
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->property("visible").toBool());
    QCOMPARE(loaded->cli.calls.size(), calls_before_reconcile);
    QVERIFY(QMetaObject::invokeMethod(dialog, "cancel"));
    QCoreApplication::processEvents();
    QCOMPARE(loaded->cli.calls.size(), calls_before_reconcile);

    click(*loaded->window, findItem(loaded->root, "gcReconcileButton"));
    QVERIFY(QMetaObject::invokeMethod(dialog, "confirm"));
    QCoreApplication::processEvents();
    QVERIFY(loaded->cli.calls.size() >= calls_before_reconcile + 2);
    QCOMPARE(loaded->cli.calls.at(calls_before_reconcile), (QStringList{"gc", "--reconcile"}));
    QCOMPARE(loaded->cli.calls.last(),
             (QStringList{"pool-stat", "--evict-events", "--evict-threshold", "10"}));

    click(*loaded->window, findItem(loaded->root, "gcReclaimButton"));
    const int calls_before_reclaim = loaded->cli.calls.size();
    QVERIFY(QMetaObject::invokeMethod(dialog, "confirm"));
    QCoreApplication::processEvents();
    QVERIFY(loaded->cli.calls.size() >= calls_before_reclaim + 2);
    QCOMPARE(loaded->cli.calls.at(calls_before_reclaim).at(0), QStringLiteral("gc"));
    QCOMPARE(loaded->cli.calls.at(calls_before_reclaim).at(1), QStringLiteral("--reclaim"));

    click(*loaded->window, findItem(loaded->root, "gcFullButton"));
    const int calls_before_full = loaded->cli.calls.size();
    QVERIFY(QMetaObject::invokeMethod(dialog, "confirm"));
    QCoreApplication::processEvents();
    QVERIFY(loaded->cli.calls.size() >= calls_before_full + 2);
    QCOMPARE(loaded->cli.calls.at(calls_before_full), QStringList{"gc"});

    click(*loaded->window, findItem(loaded->root, "lineageVerifyButton"));
    QCOMPARE(loaded->cli.calls.last(), (QStringList{"lineage", "verify"}));
    QVERIFY(loaded->root->property("verifyState").toString().contains(QStringLiteral("完整")));

    click(*loaded->window, findItem(loaded->root, "adminStatusButton"));
    QCOMPARE(loaded->cli.calls.last(), (QStringList{"admin", "status"}));
    QCOMPARE(loaded->root->property("adminPathCount").toInt(), 1);
    QCOMPARE(loaded->root->property("adminPermissionCount").toInt(), 1);
    QVERIFY(loaded->root->property("adminScaleSummary").toString().contains(QStringLiteral("version=1")));

    click(*loaded->window, findItem(loaded->root, "renewStatsButton"));
    QCOMPARE(loaded->cli.calls.last(), QStringList{"pool-stat"});
    QCOMPARE(loaded->root->property("renewEventCount").toInt(), 1);
}

void WorkspaceBCClicksTest::mainRoleGatingForWorkspaceC() {
    {
        FakeIsvnCli cli;
        FakeRoleResolver role(QStringLiteral("MEMBER"));
        auto engine = loadMain(cli, role);
        auto* tab = engine->rootObjects().first()->findChild<QQuickItem*>(QStringLiteral("workspaceCTab"));
        QVERIFY(tab != nullptr);
        QVERIFY(!tab->isEnabled());
    }
    {
        FakeIsvnCli cli;
        FakeRoleResolver role(QStringLiteral("PM"));
        auto engine = loadMain(cli, role);
        auto* root = qobject_cast<QQuickWindow*>(engine->rootObjects().first());
        QVERIFY(root != nullptr);
        auto* tab = root->findChild<QQuickItem*>(QStringLiteral("workspaceCTab"));
        QVERIFY(tab != nullptr);
        QVERIFY(tab->isEnabled());
        mouseClick(*root, tab);
        QCOMPARE(root->property("activeWorkspace").toInt(), 2);
    }
}

void WorkspaceBCClicksTest::sourceCheckNullConsistentNoHintIsNotRun() {
    // Verify that when the backend returns consistent=null and svn_copyfrom_hint=""
    // the GUI normalizes the Source Check state to NOT RUN (not REVIEW NEEDED).
    auto loaded = loadItem(QStringLiteral("qml/workspaceB/WorkspaceB.qml"));
    loaded->root->setProperty("shellVersion", QStringLiteral("Trail/Block1/RTL/v1"));
    // Configure the fake CLI to return null consistent (no copy-from comparison data).
    loaded->cli.nullConsistentReconcile = true;

    click(*loaded->window, findItem(loaded->root, "refreshEvidenceButton"));
    QCOMPARE(loaded->cli.calls.last().first(), QStringLiteral("reconcile"));

    // After populate: phase="ran", consistent=null (undefined/invalid in QVariant).
    QCOMPARE(loaded->root->property("sourceCheckPhase").toString(), QStringLiteral("ran"));

    // Invoke normalizedSourceCheckState() — must return "not_run", not "review_needed".
    QVariant stateResult;
    QVERIFY(QMetaObject::invokeMethod(loaded->root, "normalizedSourceCheckState",
                                     Qt::DirectConnection,
                                     Q_RETURN_ARG(QVariant, stateResult)));
    QCOMPARE(stateResult.toString(), QStringLiteral("not_run"));

    // Invoke sourceCheckNotRunReason() — must return a non-empty explanation.
    QVariant reasonResult;
    QVERIFY(QMetaObject::invokeMethod(loaded->root, "sourceCheckNotRunReason",
                                     Qt::DirectConnection,
                                     Q_RETURN_ARG(QVariant, reasonResult)));
    QVERIFY(!reasonResult.toString().isEmpty());
}

void WorkspaceBCClicksTest::workspaceBLineageExplorerQueriesAreReadOnly() {
    auto loaded = loadItem(QStringLiteral("qml/workspaceB/WorkspaceB.qml"));
    loaded->root->setProperty("projectPath", QStringLiteral("/tmp/chip1"));
    loaded->root->setProperty("shellVersion", QStringLiteral("P1/TOP/SYN/v3"));
    QCoreApplication::processEvents();

    QVERIFY(QMetaObject::invokeMethod(loaded->root, "runLineageQuery"));
    QCoreApplication::processEvents();
    QVERIFY(loaded->cli.calls.last().size() >= 2);
    QCOMPARE(loaded->cli.calls.last().at(0), QStringLiteral("provenance"));
    QCOMPARE(loaded->cli.calls.last().at(1), QStringLiteral("query"));
    QVERIFY(loaded->cli.calls.last().contains(QStringLiteral("--project")));
    QVERIFY(loaded->root->property("lineageResultCount").toInt() >= 4);
    QVERIFY(loaded->root->property("lineageSummary").toString().contains(QStringLiteral("declared_provenance")));

    loaded->root->setProperty("lineageMode", QStringLiteral("file"));
    loaded->root->setProperty("fileQueryPath", QStringLiteral("src/top.v"));
    loaded->root->setProperty("fileQueryType", QStringLiteral("parent"));
    QVERIFY(QMetaObject::invokeMethod(loaded->root, "runLineageQuery"));
    QCoreApplication::processEvents();
    QVERIFY(loaded->cli.calls.last().size() >= 2);
    QCOMPARE(loaded->cli.calls.last().at(0), QStringLiteral("file"));
    QCOMPARE(loaded->cli.calls.last().at(1), QStringLiteral("query"));
    QCOMPARE(loaded->root->property("multiCandidateCount").toInt(), 2);
    QVERIFY(loaded->root->property("lineageSummary").toString().contains(QStringLiteral("declared_inventory")));

    for (const auto& call : loaded->cli.calls) {
        QVERIFY(!call.contains(QStringLiteral("checkout")));
        QVERIFY(!call.contains(QStringLiteral("edit")));
        QVERIFY(!call.contains(QStringLiteral("checkin")));
        QVERIFY(!call.contains(QStringLiteral("gc")));
    }
}

void WorkspaceBCClicksTest::workspaceBLineageGraphRendersRootSentinel() {
    auto loaded = loadItem(QStringLiteral("qml/workspaceB/WorkspaceB.qml"));
    loaded->root->setProperty("projectPath", QStringLiteral("/tmp/chip1"));
    loaded->root->setProperty("versionQueryTarget", QStringLiteral("P1/TOP/RTL/rooted"));
    QCoreApplication::processEvents();

    QVERIFY(QMetaObject::invokeMethod(loaded->root, "runLineageQuery"));
    QCoreApplication::processEvents();

    QCOMPARE(loaded->cli.calls.last().at(0), QStringLiteral("provenance"));
    QCOMPARE(loaded->root->property("lineageResultCount").toInt(), 2);
    QVERIFY(loaded->root->property("lineageSummary").toString().contains(QStringLiteral("ROOT")));
    QCOMPARE(loaded->root->property("selectedLineageCanonical").toString(),
             QStringLiteral("isvn://chip1/ROOT"));
    QCOMPARE(loaded->root->property("lineageStatus").toString(), QStringLiteral("ok"));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    WorkspaceBCClicksTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_workspace_bc_clicks.moc"
