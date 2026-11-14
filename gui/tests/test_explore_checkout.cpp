// Explore explicit checkout test.
//
// Loads the real ProductShell in read mode with a fake operation bridge and
// drives the Explore page.  The fake records every command on both bridge
// lanes; it runs no process and touches no file.
#include <QFile>
#include <QJsonDocument>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QVariant>

#include <memory>

namespace {
const QString kTarget = QStringLiteral("P1-trial/cpu/200-LOGIC/v2");
const QString kParent = QStringLiteral("P1-trial/cpu/200-LOGIC/v1");
const QString kLocal = QStringLiteral("P1-trial/cpu/000-DATA/v1");
const QString kWorkspace = QStringLiteral("/demo/learner-workspace");
}  // namespace

class FakeExploreCli final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(int activeMutations READ activeMutations NOTIFY lanesChanged)
    Q_PROPERTY(int activeReads READ activeReads NOTIFY lanesChanged)
    Q_PROPERTY(QString maintenanceRole MEMBER maintenanceRole NOTIFY maintenanceRoleChanged)

public:
    struct Call {
        QString operationId;
        QString target;
        QStringList arguments;
        bool mutation = false;
    };

    QVector<Call> calls;            // target-pinned operation lane
    QVector<QStringList> legacy;    // single-flight lane used by the embedded body
    QStringList repository{kLocal, kParent, kTarget};
    QStringList local{kLocal};
    QString checkoutError;          // non-empty: checkout fails with this code
    bool checkoutUnconfirmed = false;  // checkout "succeeds" without the CLI's confirmation
    bool deferCheckout = false;
    bool refuseReads = false;       // bridge rejects a read synchronously
    int failWorkspaces = 0;         // the next N `workspaces` reads answer with an error
    QString workspacesShape = QStringLiteral("ok");  // shape of a successful `workspaces` answer
    // Non-empty: a successful `workspaces` answer is this JSON text, converted the
    // way IsvnCli converts CLI output (QJsonDocument::fromJson(...).toVariant()).
    QByteArray workspacesJson;
    bool holdProjectReads = false;  // keep the read controller's own reads unanswered
    QString maintenanceRole;

    bool running() const { return false; }
    int activeMutations() const { return 0; }
    int activeReads() const { return 0; }

    Q_INVOKABLE bool runJsonForOperation(const QString& operationId, const QString& target,
                                         const QVariant& arguments, const QString&, bool mutation) {
        QStringList args;
        for (const QVariant& value : arguments.toList()) args.push_back(value.toString());
        const Call call{operationId, target, args, mutation};
        calls.push_back(call);
        if (refuseReads && !mutation) {
            emit operationFailedJson(operationId, target, error(QStringLiteral("READ_LANE_BUSY")));
            return false;
        }
        if (args.value(0) == QStringLiteral("checkout") && deferCheckout) {
            deferred_ = call;
            return true;
        }
        if (holdProjectReads && target == QStringLiteral("project-read")) {
            held_.push_back(call);
            return true;
        }
        QTimer::singleShot(0, this, [this, call]() { finish(call); });
        return true;
    }

    Q_INVOKABLE void runJson(const QVariant& arguments) {
        QStringList args;
        for (const QVariant& value : arguments.toList()) args.push_back(value.toString());
        legacy.push_back(args);
        QTimer::singleShot(0, this, [this]() { emit commandSucceeded(QVariantMap{}); });
    }

    void finishDeferredCheckout() {
        if (deferred_.operationId.isEmpty()) return;
        const Call call = deferred_;
        deferred_ = {};
        finish(call);
    }

    int heldReads() const { return held_.size(); }

    // Answers the oldest held read with the inventory as it is now.
    void releaseHeldRead() {
        if (held_.isEmpty()) return;
        const Call call = held_.takeFirst();
        finish(call);
    }

    // A completion for an operation nobody is waiting for any more.
    void emitStrayInventory(const QString& operationId) {
        emit operationSucceeded(operationId, QStringLiteral("project-read"), inventory());
    }

    int count(const QString& command) const {
        int total = 0;
        for (const Call& call : calls)
            if (call.arguments.value(0) == command) ++total;
        return total;
    }

    QStringList commandsIssued() const {
        QStringList commands;
        for (const Call& call : calls) commands.push_back(call.arguments.value(0));
        for (const QStringList& args : legacy) commands.push_back(args.value(0));
        return commands;
    }

signals:
    void runningChanged();
    void lanesChanged();
    void maintenanceRoleChanged();
    void operationSucceeded(const QString& operationId, const QString& targetVersion,
                            const QVariant& json);
    void operationFailedJson(const QString& operationId, const QString& targetVersion,
                             const QVariant& envelope);
    void commandSucceeded(const QVariant& json);
    void commandFailed(const QString& message);

private:
    Call deferred_;
    QVector<Call> held_;

    QVariantMap inventory() const {
        QVariantList items;
        for (const QString& version : local) {
            const QStringList parts = version.split('/');
            items.push_back(QVariantMap{
                {QStringLiteral("workspace_id"), QStringLiteral("ws-") + parts.value(2) + parts.value(3)},
                {QStringLiteral("version"), parts.value(3)},
                {QStringLiteral("phase"), parts.value(0)},
                {QStringLiteral("block"), parts.value(1)},
                {QStringLiteral("stage"), parts.value(2)},
                {QStringLiteral("repository_path"), version},
                {QStringLiteral("subdir"), version},
                {QStringLiteral("working_copy"), kWorkspace + QStringLiteral("/") + version},
                {QStringLiteral("counts"), QVariantMap{{QStringLiteral("changed"), 0},
                                                         {QStringLiteral("conflict"), 0},
                                                         {QStringLiteral("local"), 0}}},
                {QStringLiteral("health"), QStringLiteral("ok")},
                {QStringLiteral("issues"), QVariantList{}}});
        }
        return QVariantMap{{QStringLiteral("schema_version"), QStringLiteral("workspaces/v1")},
                           {QStringLiteral("project"),
                            QVariantMap{{QStringLiteral("name"), QStringLiteral("demo")}}},
                           {QStringLiteral("workspaces"), items},
                           {QStringLiteral("issues"), QVariantList{}}};
    }

    // A successful answer that is not a readable workspaces/v1 inventory.
    QVariant shapedInventory() const {
        QVariantMap answer = inventory();
        const QString shape = workspacesShape;
        const auto element = [](const QVariant& value) { return QVariantList{value}; };
        if (shape == QStringLiteral("missingField")) answer.remove(QStringLiteral("workspaces"));
        else if (shape == QStringLiteral("nullField")) answer.insert(QStringLiteral("workspaces"), QVariant());
        else if (shape == QStringLiteral("stringField")) answer.insert(QStringLiteral("workspaces"), QStringLiteral("[]"));
        else if (shape == QStringLiteral("objectField")) answer.insert(QStringLiteral("workspaces"), QVariantMap{});
        // Objects that only look like a list.
        else if (shape == QStringLiteral("lengthOnlyObject"))
            answer.insert(QStringLiteral("workspaces"), QVariantMap{{QStringLiteral("length"), 0}});
        else if (shape == QStringLiteral("arrayLikeObject"))
            answer.insert(QStringLiteral("workspaces"),
                          QVariantMap{{QStringLiteral("length"), 1},
                                      {QStringLiteral("0"), QVariantMap{{QStringLiteral("repository_path"), kParent}}}});
        else if (shape == QStringLiteral("keyedObject"))
            answer.insert(QStringLiteral("workspaces"),
                          QVariantMap{{QStringLiteral("a"), QVariantMap{{QStringLiteral("repository_path"), kParent}}}});
        else if (shape == QStringLiteral("numberField")) answer.insert(QStringLiteral("workspaces"), 0);
        else if (shape == QStringLiteral("falseField")) answer.insert(QStringLiteral("workspaces"), false);
        else if (shape == QStringLiteral("elementNull")) answer.insert(QStringLiteral("workspaces"), element(QVariant()));
        else if (shape == QStringLiteral("elementString")) answer.insert(QStringLiteral("workspaces"), element(kTarget));
        else if (shape == QStringLiteral("elementNoPath"))
            answer.insert(QStringLiteral("workspaces"),
                          element(QVariantMap{{QStringLiteral("workspace_id"), QStringLiteral("ws-x")}}));
        else if (shape == QStringLiteral("elementPathNumber"))
            answer.insert(QStringLiteral("workspaces"),
                          element(QVariantMap{{QStringLiteral("repository_path"), 7}}));
        else if (shape == QStringLiteral("elementPathEmpty"))
            answer.insert(QStringLiteral("workspaces"),
                          element(QVariantMap{{QStringLiteral("repository_path"), QString()}}));
        else if (shape == QStringLiteral("otherSchema"))
            answer.insert(QStringLiteral("schema_version"), QStringLiteral("workspaces/v2"));
        else if (shape == QStringLiteral("noSchema")) answer.remove(QStringLiteral("schema_version"));
        else if (shape == QStringLiteral("emptyObject")) return QVariantMap{};
        else if (shape == QStringLiteral("topLevelList")) return QVariantList{};
        return answer;
    }

    static QVariantMap error(const QString& code) {
        return QVariantMap{{QStringLiteral("schema_version"), QStringLiteral("work-error/v1")},
                           {QStringLiteral("ok"), false},
                           {QStringLiteral("error"), QVariantMap{{QStringLiteral("code"), code}}}};
    }

    void finish(const Call& call) {
        const QString command = call.arguments.value(0);
        if (command == QStringLiteral("workspaces")) {
            if (failWorkspaces > 0) {
                --failWorkspaces;
                emit operationFailedJson(call.operationId, call.target, error(QStringLiteral("SVN_QUERY_FAILED")));
                return;
            }
            if (!workspacesJson.isEmpty()) {
                emit operationSucceeded(call.operationId, call.target,
                                        QJsonDocument::fromJson(workspacesJson).toVariant());
                return;
            }
            emit operationSucceeded(call.operationId, call.target,
                                    workspacesShape == QStringLiteral("ok") ? QVariant(inventory())
                                                                            : shapedInventory());
            return;
        }
        if (command == QStringLiteral("work-status")) {
            emit operationSucceeded(call.operationId, call.target,
                                    QVariantMap{{QStringLiteral("version"), call.arguments.value(1)},
                                                {QStringLiteral("entries"), QVariantList{}}});
            return;
        }
        if (command == QStringLiteral("branches")) {
            QVariantList items;
            for (const QString& version : repository)
                items.push_back(QVariantMap{{QStringLiteral("path"), version},
                                            {QStringLiteral("removed"), false}});
            emit operationSucceeded(call.operationId, call.target,
                                    QVariantMap{{QStringLiteral("branches"), items}});
            return;
        }
        if (command == QStringLiteral("checkout")) {
            if (!checkoutError.isEmpty()) {
                emit operationFailedJson(call.operationId, call.target, error(checkoutError));
                return;
            }
            if (checkoutUnconfirmed) {
                emit operationSucceeded(call.operationId, call.target, QVariantMap{});
                return;
            }
            const QString version = call.arguments.value(1);
            if (!local.contains(version)) local.push_back(version);
            emit operationSucceeded(
                call.operationId, call.target,
                QVariantMap{{QStringLiteral("result"), QStringLiteral("checked_out")},
                            {QStringLiteral("version"), version},
                            {QStringLiteral("path"), kWorkspace + QStringLiteral("/") + version}});
            return;
        }
        emit operationSucceeded(call.operationId, call.target, QVariantMap{});
    }
};

class ExploreCheckoutTest final : public QObject {
    Q_OBJECT

    struct Loaded {
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem* shell = nullptr;
        QObject* read = nullptr;
        QObject* explore = nullptr;
        QObject* controller = nullptr;
        QObject* checkoutButton = nullptr;
        QObject* setActiveButton = nullptr;
        QObject* confirmDialog = nullptr;
        QObject* body = nullptr;
    };

    static QString source(const QString& relative) {
        QFile file(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/") + relative);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(file.readAll());
    }

    static bool click(QObject* button) {
        return button != nullptr && button->property("enabled").toBool() &&
               button->property("visible").toBool() && QMetaObject::invokeMethod(button, "clicked");
    }

    static QString phase(const Loaded& l) { return l.controller->property("phase").toString(); }
    static QString active(const Loaded& l) { return l.read->property("activeVersionId").toString(); }

    static bool waitForPhase(const Loaded& l, const QString& expected) {
        for (int attempt = 0; attempt < 100 && phase(l) != expected; ++attempt) QTest::qWait(10);
        return phase(l) == expected;
    }

    static void settle() { QTest::qWait(60); }

    static bool hasLocal(const Loaded& l, const QString& version) {
        const QVariantList versions = l.read->property("localVersions").toList();
        for (const QVariant& item : versions)
            if (item.toMap().value(QStringLiteral("id")).toString() == version) return true;
        return false;
    }

    std::unique_ptr<Loaded> load(FakeExploreCli* cli) {
        auto l = std::make_unique<Loaded>();
        l->engine = std::make_unique<QQmlEngine>();
        l->engine->addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        // The embedded Explore body reaches the bridge through this context name.
        l->engine->rootContext()->setContextProperty(QStringLiteral("isvnCli"), cli);
        QQmlComponent component(l->engine.get(),
                                QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                    QStringLiteral("/qml/shell/ProductShell.qml")));
        QObject* created = component.create();
        if (!created) qFatal("%s", qPrintable(component.errorString()));
        l->shell = qobject_cast<QQuickItem*>(created);
        if (!l->shell) qFatal("ProductShell root is not an item");
        l->window = std::make_unique<QQuickWindow>();
        l->window->resize(1600, 900);
        l->shell->setParent(l->window.get());
        l->shell->setParentItem(l->window->contentItem());
        l->shell->setSize(l->window->size());
        l->shell->setProperty("isvnCli", QVariant::fromValue(static_cast<QObject*>(cli)));
        l->shell->setProperty("workspaceRoot", kWorkspace);
        // Assertions on visible text are written against the English strings.
        if (auto* i18n = l->shell->findChild<QObject*>(QStringLiteral("workI18n")))
            i18n->setProperty("language", QStringLiteral("en"));
        l->shell->setProperty("readMode", true);
        l->window->show();
        if (!QTest::qWaitForWindowExposed(l->window.get())) qFatal("window was not exposed");

        l->read = l->shell->findChild<QObject*>(QStringLiteral("workReadController"));
        l->explore = l->shell->findChild<QObject*>(QStringLiteral("explorePageLoader"));
        l->controller = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutController"));
        l->checkoutButton = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckout"));
        l->setActiveButton = l->shell->findChild<QObject*>(QStringLiteral("exploreSetActiveOpenWork"));
        l->confirmDialog = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutConfirm"));
        l->body = l->shell->findChild<QObject*>(QStringLiteral("embeddedExploreBody"));
        if (!l->read || !l->explore || !l->controller || !l->checkoutButton || !l->setActiveButton ||
            !l->confirmDialog || !l->body)
            qFatal("ProductShell did not create the Explore checkout objects");

        if (!QMetaObject::invokeMethod(l->read, "refresh")) qFatal("refresh unavailable");
        for (int attempt = 0; attempt < 100; ++attempt) {
            const QString state = l->read->property("state").toString();
            if (state == QStringLiteral("ready") || state == QStringLiteral("empty")) break;
            QTest::qWait(10);
        }
        l->shell->setProperty("currentPage", QStringLiteral("Explore"));
        settle();
        cli->calls.clear();
        cli->legacy.clear();
        return l;
    }

    static void inspect(const Loaded& l, const QString& version) {
        l.explore->setProperty("inspectedVersion", version);
        settle();
    }

private slots:
    void selectingAVersionIssuesNoCheckoutAndKeepsActive();
    void checkoutIsOfferedOnlyForAVersionThatIsNotLocal();
    void headerFieldAndEmbeddedBodyShareOneSelection();
    void cancelIssuesNoCheckout();
    void confirmIssuesExactlyOneCheckoutAndLeavesActiveAlone();
    void repeatedConfirmAndRequestWhilePendingAreIgnored();
    void setActiveIsASeparateExplicitAction();
    void setActiveOnAVersionThatIsNotLocalExplainsAndChangesNothing();
    void failedCheckoutIsReportedAndRecoverable();
    void unconfirmedResultIsNotASuccess();
    void versionMissingFromRepositoryNeverReachesConfirm();
    void versionAlreadyLocalNeverReachesConfirm();
    void refusedReadIsReportedAndRecoverable();
    void failedSetActiveIsNotCompletedByALaterCheckout();
    void failedSetActiveDoesNotChangeAnExistingActiveAfterCheckout();
    void lateAnswerToASetActiveCannotCompleteItAfterCheckout();
    void unreadableLocalInventoryNeverReachesConfirm_data();
    void unreadableLocalInventoryNeverReachesConfirm();
    void readableInventoryDecidesWhetherCheckoutIsOffered();
    void cliInventoryTextIsJudgedAfterTheBridgeConversion_data();
    void cliInventoryTextIsJudgedAfterTheBridgeConversion();
    void embeddedBodyReceivesTheProjectPath();
    void controllerSourceHasNoHiddenSideEffects();
    void shellShowsSuppliedBuildIdentityOnly();
};

void ExploreCheckoutTest::selectingAVersionIssuesNoCheckoutAndKeepsActive() {
    FakeExploreCli cli;
    auto l = load(&cli);
    QCOMPARE(active(*l), QString());
    inspect(*l, kTarget);
    inspect(*l, kParent);
    inspect(*l, kTarget);
    QCOMPARE(phase(*l), QStringLiteral("idle"));
    QCOMPARE(active(*l), QString());
    QVERIFY(!hasLocal(*l, kTarget));
    for (const QString& command : cli.commandsIssued()) {
        QVERIFY2(command != QStringLiteral("checkout") && command != QStringLiteral("create") &&
                 command != QStringLiteral("checkout-ancestor") && command != QStringLiteral("ancestry"),
                 qPrintable(command));
    }
    QCOMPARE(cli.calls.size(), 0);
}

void ExploreCheckoutTest::checkoutIsOfferedOnlyForAVersionThatIsNotLocal() {
    FakeExploreCli cli;
    auto l = load(&cli);
    // Nothing selected.
    QCOMPARE(l->checkoutButton->property("visible").toBool(), false);
    // Not local, well formed: offered and enabled.
    inspect(*l, kTarget);
    QCOMPARE(l->checkoutButton->property("visible").toBool(), true);
    QCOMPARE(l->checkoutButton->property("enabled").toBool(), true);
    // Already local: not offered.
    inspect(*l, kLocal);
    QCOMPARE(l->explore->property("inspectedIsLocal").toBool(), true);
    QCOMPARE(l->checkoutButton->property("visible").toBool(), false);
    // Not a version path: shown but disabled, with an explanation.
    inspect(*l, QStringLiteral("P1-trial/cpu"));
    QCOMPARE(l->checkoutButton->property("enabled").toBool(), false);
    QVERIFY(!click(l->checkoutButton));
    auto* status = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutStatus"));
    QVERIFY(status != nullptr);
    QVERIFY(status->property("message").toString().contains(QStringLiteral("phase/block/stage/version")));
    for (const QString& bad : {QStringLiteral("a/b/c/d/e"), QStringLiteral("a//c/d"),
                               QStringLiteral("a/../c/d"), QStringLiteral("a/b c/d/e"),
                               QStringLiteral("/a/b/c/d")}) {
        inspect(*l, bad);
        QVERIFY2(!l->checkoutButton->property("enabled").toBool(), qPrintable(bad));
    }
    QCOMPARE(cli.calls.size(), 0);
}

void ExploreCheckoutTest::headerFieldAndEmbeddedBodyShareOneSelection() {
    FakeExploreCli cli;
    auto l = load(&cli);
    // A choice made inside the embedded body becomes the inspected Version.
    l->body->setProperty("versionFilter", kTarget);
    settle();
    QCOMPARE(l->explore->property("inspectedVersion").toString(), kTarget);
    auto* field = l->shell->findChild<QObject*>(QStringLiteral("exploreInspectedVersion"));
    QVERIFY(field != nullptr);
    QCOMPARE(field->property("text").toString(), kTarget);
    // A choice made in the header reaches the body, also after the body chose once.
    inspect(*l, kParent);
    QCOMPARE(l->body->property("versionFilter").toString(), kParent);
    l->body->setProperty("versionFilter", kLocal);
    settle();
    QCOMPARE(l->explore->property("inspectedVersion").toString(), kLocal);
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
}

void ExploreCheckoutTest::cancelIssuesNoCheckout() {
    FakeExploreCli cli;
    auto l = load(&cli);
    inspect(*l, kTarget);
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    settle();
    QCOMPARE(l->confirmDialog->property("visible").toBool(), true);
    const QString body = l->confirmDialog->property("body").toString();
    QVERIFY2(body.contains(kTarget), qPrintable(body));
    QVERIFY2(body.contains(kWorkspace + QStringLiteral("/") + kTarget), qPrintable(body));
    // The pre-checks are reads of the repository list and the local inventory.
    QCOMPARE(cli.count(QStringLiteral("branches")), 1);
    QCOMPARE(cli.count(QStringLiteral("workspaces")), 1);
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);

    // Cancel through the dialog's own button, as a user does.
    auto* cancel_button = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutCancelButton"));
    QVERIFY(cancel_button != nullptr);
    QVERIFY(l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutConfirmButton")) != nullptr);
    QVERIFY(QMetaObject::invokeMethod(cancel_button, "clicked"));
    settle();
    QCOMPARE(phase(*l), QStringLiteral("idle"));
    QCOMPARE(l->confirmDialog->property("visible").toBool(), false);
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
    QCOMPARE(active(*l), QString());
    QVERIFY(!hasLocal(*l, kTarget));
}

void ExploreCheckoutTest::confirmIssuesExactlyOneCheckoutAndLeavesActiveAlone() {
    FakeExploreCli cli;
    auto l = load(&cli);
    inspect(*l, kTarget);
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    // Confirm through the dialog's own button, as a user does.
    auto* confirm_button = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutConfirmButton"));
    QVERIFY(confirm_button != nullptr);
    QVERIFY(QMetaObject::invokeMethod(confirm_button, "clicked"));
    QVERIFY(waitForPhase(*l, QStringLiteral("done")));
    settle();

    QCOMPARE(cli.count(QStringLiteral("checkout")), 1);
    const FakeExploreCli::Call* checkout = nullptr;
    for (const auto& call : cli.calls)
        if (call.arguments.value(0) == QStringLiteral("checkout")) checkout = &call;
    QVERIFY(checkout != nullptr);
    QCOMPARE(checkout->arguments, (QStringList{QStringLiteral("checkout"), kTarget}));
    QCOMPARE(checkout->target, kTarget);
    QCOMPARE(checkout->mutation, true);
    // Only the requested Version: no parent, no create, no lineage query.
    for (const QString& command : cli.commandsIssued())
        QVERIFY2(command != QStringLiteral("create") && command != QStringLiteral("checkout-ancestor") &&
                 command != QStringLiteral("ancestry"), qPrintable(command));
    for (const auto& call : cli.calls)
        QVERIFY2(!call.arguments.contains(kParent), qPrintable(call.arguments.join(' ')));

    QCOMPARE(l->controller->property("resultPath").toString(), kWorkspace + QStringLiteral("/") + kTarget);
    QVERIFY(hasLocal(*l, kTarget));
    QVERIFY(!hasLocal(*l, kParent));
    QCOMPARE(active(*l), QString());
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Explore"));
    QCOMPARE(l->checkoutButton->property("visible").toBool(), false);
    auto* status = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutStatus"));
    QVERIFY(status->property("message").toString().contains(kTarget));
}

void ExploreCheckoutTest::repeatedConfirmAndRequestWhilePendingAreIgnored() {
    FakeExploreCli cli;
    cli.deferCheckout = true;
    auto l = load(&cli);
    inspect(*l, kTarget);
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    QVERIFY(QMetaObject::invokeMethod(l->confirmDialog, "confirm"));
    // A second confirmation (double click) and further requests do nothing.
    QVariant accepted;
    QVERIFY(QMetaObject::invokeMethod(l->controller, "confirm", Q_RETURN_ARG(QVariant, accepted)));
    QCOMPARE(accepted.toBool(), false);
    QVERIFY(QMetaObject::invokeMethod(l->controller, "request", Q_RETURN_ARG(QVariant, accepted),
                                      Q_ARG(QVariant, QVariant(kParent))));
    QCOMPARE(accepted.toBool(), false);
    settle();
    QCOMPARE(phase(*l), QStringLiteral("running"));
    QCOMPARE(l->controller->property("targetVersion").toString(), kTarget);
    QCOMPARE(cli.count(QStringLiteral("checkout")), 1);
    // While it runs the page offers no second Checkout, even for another Version.
    inspect(*l, kParent);
    QCOMPARE(l->checkoutButton->property("enabled").toBool(), false);
    QVERIFY(!click(l->checkoutButton));
    QCOMPARE(l->setActiveButton->property("enabled").toBool(), false);
    QCOMPARE(phase(*l), QStringLiteral("running"));
    QVERIFY(!hasLocal(*l, kTarget));
    QCOMPARE(active(*l), QString());

    cli.finishDeferredCheckout();
    QVERIFY(waitForPhase(*l, QStringLiteral("done")));
    settle();
    QCOMPARE(cli.count(QStringLiteral("checkout")), 1);
    // The result belongs to the Version that was requested, not the one now shown.
    QCOMPARE(l->controller->property("targetVersion").toString(), kTarget);
    QVERIFY(hasLocal(*l, kTarget));
    QVERIFY(!hasLocal(*l, kParent));
    QCOMPARE(active(*l), QString());
}

void ExploreCheckoutTest::setActiveIsASeparateExplicitAction() {
    FakeExploreCli cli;
    auto l = load(&cli);
    inspect(*l, kTarget);
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    QVERIFY(QMetaObject::invokeMethod(l->confirmDialog, "confirm"));
    QVERIFY(waitForPhase(*l, QStringLiteral("done")));
    settle();
    QCOMPARE(active(*l), QString());
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Explore"));

    QVERIFY(click(l->setActiveButton));
    for (int attempt = 0; attempt < 100 && active(*l) != kTarget; ++attempt) QTest::qWait(10);
    QCOMPARE(active(*l), kTarget);
    settle();
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Work"));
    QCOMPARE(cli.count(QStringLiteral("checkout")), 1);
    // The "Active is unchanged" note no longer applies and is gone.
    QCOMPARE(phase(*l), QStringLiteral("idle"));
}

void ExploreCheckoutTest::setActiveOnAVersionThatIsNotLocalExplainsAndChangesNothing() {
    FakeExploreCli cli;
    auto l = load(&cli);
    inspect(*l, kTarget);
    QVERIFY(click(l->setActiveButton));
    settle();
    settle();
    QCOMPARE(active(*l), QString());
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Explore"));
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
    auto* status = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutStatus"));
    QCOMPARE(status->property("severity").toString(), QStringLiteral("warn"));
    QVERIFY(status->property("message").toString().contains(QStringLiteral("Checkout this version first")));
    // The note belongs to that Version only.
    inspect(*l, kParent);
    QVERIFY(!status->property("message").toString().contains(QStringLiteral("Checkout this version first")));
}

void ExploreCheckoutTest::failedCheckoutIsReportedAndRecoverable() {
    FakeExploreCli cli;
    cli.checkoutError = QStringLiteral("SVN_QUERY_FAILED");
    auto l = load(&cli);
    inspect(*l, kTarget);
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    QVERIFY(QMetaObject::invokeMethod(l->confirmDialog, "confirm"));
    QVERIFY(waitForPhase(*l, QStringLiteral("failed")));
    settle();
    QCOMPARE(l->controller->property("errorCode").toString(), QStringLiteral("SVN_QUERY_FAILED"));
    QCOMPARE(l->controller->property("resultPath").toString(), QString());
    QVERIFY(!hasLocal(*l, kTarget));
    QCOMPARE(active(*l), QString());
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Explore"));
    auto* status = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutStatus"));
    QCOMPARE(status->property("severity").toString(), QStringLiteral("danger"));
    QVERIFY(status->property("message").toString().contains(QStringLiteral("did not complete")));
    QVERIFY(status->property("message").toString().contains(QStringLiteral("Nothing was activated")));

    // The page is usable again: the same Version can be requested once more.
    cli.checkoutError.clear();
    QCOMPARE(l->checkoutButton->property("enabled").toBool(), true);
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    QVERIFY(QMetaObject::invokeMethod(l->confirmDialog, "confirm"));
    QVERIFY(waitForPhase(*l, QStringLiteral("done")));
    settle();
    QCOMPARE(cli.count(QStringLiteral("checkout")), 2);
    QVERIFY(hasLocal(*l, kTarget));
    QCOMPARE(active(*l), QString());
}

void ExploreCheckoutTest::unconfirmedResultIsNotASuccess() {
    FakeExploreCli cli;
    cli.checkoutUnconfirmed = true;
    auto l = load(&cli);
    inspect(*l, kTarget);
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    QVERIFY(QMetaObject::invokeMethod(l->confirmDialog, "confirm"));
    QVERIFY(waitForPhase(*l, QStringLiteral("failed")));
    QCOMPARE(l->controller->property("errorCode").toString(), QStringLiteral("UNEXPECTED_RESULT"));
    QCOMPARE(active(*l), QString());
}

void ExploreCheckoutTest::versionMissingFromRepositoryNeverReachesConfirm() {
    FakeExploreCli cli;
    auto l = load(&cli);
    inspect(*l, QStringLiteral("P1-trial/cpu/200-LOGIC/v9"));
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("failed")));
    settle();
    QCOMPARE(l->controller->property("errorCode").toString(), QStringLiteral("VERSION_NOT_IN_REPOSITORY"));
    QCOMPARE(l->confirmDialog->property("visible").toBool(), false);
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
    QCOMPARE(active(*l), QString());
}

void ExploreCheckoutTest::versionAlreadyLocalNeverReachesConfirm() {
    FakeExploreCli cli;
    auto l = load(&cli);
    inspect(*l, kTarget);
    QCOMPARE(l->checkoutButton->property("visible").toBool(), true);
    // The Version became local outside this window; the page has not reloaded yet.
    cli.local.push_back(kTarget);
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("failed")));
    settle();
    QCOMPARE(l->controller->property("errorCode").toString(), QStringLiteral("ALREADY_LOCAL"));
    QCOMPARE(l->confirmDialog->property("visible").toBool(), false);
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
    // The inventory is reloaded, so the page now shows the real state.
    QVERIFY(hasLocal(*l, kTarget));
    QCOMPARE(l->checkoutButton->property("visible").toBool(), false);
    QCOMPARE(active(*l), QString());
}

void ExploreCheckoutTest::refusedReadIsReportedAndRecoverable() {
    FakeExploreCli cli;
    auto l = load(&cli);
    inspect(*l, kTarget);
    cli.refuseReads = true;
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("failed")));
    QCOMPARE(l->controller->property("errorCode").toString(), QStringLiteral("READ_LANE_BUSY"));
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
    cli.refuseReads = false;
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    QVERIFY(QMetaObject::invokeMethod(l->confirmDialog, "cancel"));
    settle();
    QCOMPARE(phase(*l), QStringLiteral("idle"));
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
}

// A Set Active whose inventory read fails must end there.  A later Checkout of
// the same Version reloads the inventory; that reload must not finish it.
void ExploreCheckoutTest::failedSetActiveIsNotCompletedByALaterCheckout() {
    FakeExploreCli cli;
    auto l = load(&cli);
    QSignalSpy resolved(l->read, SIGNAL(explicitActiveResolved(bool,QString)));
    QVERIFY(resolved.isValid());
    inspect(*l, kTarget);

    // 1. The user asks for Set Active; the read it starts fails.
    cli.failWorkspaces = 1;
    QVERIFY(click(l->setActiveButton));
    settle();
    QCOMPARE(cli.failWorkspaces, 0);
    QCOMPARE(active(*l), QString());
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Explore"));

    // 2. The user then checks the Version out, and does nothing else.
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    auto* confirm_button = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutConfirmButton"));
    QVERIFY(QMetaObject::invokeMethod(confirm_button, "clicked"));
    // Wait for the inventory reload that follows the checkout, not for a phase.
    for (int attempt = 0; attempt < 100 && !hasLocal(*l, kTarget); ++attempt) QTest::qWait(10);
    settle();
    settle();
    QCOMPARE(cli.count(QStringLiteral("checkout")), 1);
    QVERIFY(hasLocal(*l, kTarget));
    // Active is untouched and the page did not move to Work.
    QCOMPARE(active(*l), QString());
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Explore"));
    for (const QList<QVariant>& emission : resolved)
        QVERIFY2(!emission.value(0).toBool(), "Set Active was completed without a new request");
    QCOMPARE(phase(*l), QStringLiteral("done"));

    // 3. Only a new, explicit Set Active changes Active and opens Work.
    resolved.clear();
    QVERIFY(click(l->setActiveButton));
    for (int attempt = 0; attempt < 100 && active(*l) != kTarget; ++attempt) QTest::qWait(10);
    QCOMPARE(active(*l), kTarget);
    settle();
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Work"));
    QCOMPARE(resolved.size(), 1);
    QCOMPARE(resolved.at(0).value(0).toBool(), true);
    QCOMPARE(resolved.at(0).value(1).toString(), kTarget);
    QCOMPARE(cli.count(QStringLiteral("checkout")), 1);
}

// With an Active Version already set: a failed Set Active for another local
// Version must not be finished by the reload that follows an unrelated Checkout.
void ExploreCheckoutTest::failedSetActiveDoesNotChangeAnExistingActiveAfterCheckout() {
    FakeExploreCli cli;
    cli.local = QStringList{kLocal, kParent};
    auto l = load(&cli);
    inspect(*l, kLocal);
    QVERIFY(click(l->setActiveButton));
    for (int attempt = 0; attempt < 100 && active(*l) != kLocal; ++attempt) QTest::qWait(10);
    QCOMPARE(active(*l), kLocal);
    l->shell->setProperty("currentPage", QStringLiteral("Explore"));
    settle();

    QSignalSpy resolved(l->read, SIGNAL(explicitActiveResolved(bool,QString)));
    inspect(*l, kParent);
    cli.failWorkspaces = 1;
    QVERIFY(click(l->setActiveButton));
    settle();
    QCOMPARE(active(*l), kLocal);

    inspect(*l, kTarget);
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    auto* confirm_button = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutConfirmButton"));
    QVERIFY(QMetaObject::invokeMethod(confirm_button, "clicked"));
    QVERIFY(waitForPhase(*l, QStringLiteral("done")));
    settle();
    settle();
    QVERIFY(hasLocal(*l, kTarget));
    QCOMPARE(active(*l), kLocal);
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Explore"));
    for (const QList<QVariant>& emission : resolved)
        QVERIFY2(!emission.value(0).toBool(), "a failed Set Active was completed later");
}

// The read behind a Set Active is still unanswered when the user checks the
// Version out.  Its answer arrives after the Checkout and already lists the
// Version; it must not complete the earlier request.
void ExploreCheckoutTest::lateAnswerToASetActiveCannotCompleteItAfterCheckout() {
    FakeExploreCli cli;
    auto l = load(&cli);
    QSignalSpy resolved(l->read, SIGNAL(explicitActiveResolved(bool,QString)));
    inspect(*l, kTarget);

    cli.holdProjectReads = true;
    QVERIFY(click(l->setActiveButton));
    settle();
    QCOMPARE(cli.heldReads(), 1);

    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    auto* confirm_button = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutConfirmButton"));
    QVERIFY(QMetaObject::invokeMethod(confirm_button, "clicked"));
    QVERIFY(waitForPhase(*l, QStringLiteral("done")));
    settle();
    QCOMPARE(cli.count(QStringLiteral("checkout")), 1);

    // The old read is answered now; the inventory it carries includes the Version.
    cli.releaseHeldRead();
    settle();
    while (cli.heldReads() > 0) {
        cli.releaseHeldRead();
        settle();
    }
    QVERIFY(hasLocal(*l, kTarget));
    QCOMPARE(active(*l), QString());
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Explore"));
    for (const QList<QVariant>& emission : resolved)
        QVERIFY2(!emission.value(0).toBool(), "a late answer completed an earlier Set Active");

    // A completion nobody is waiting for changes nothing either.
    cli.emitStrayInventory(QStringLiteral("ui-op-0-0"));
    settle();
    QCOMPARE(active(*l), QString());
    QCOMPARE(l->shell->property("currentPage").toString(), QStringLiteral("Explore"));

    // A fresh request still works.
    cli.holdProjectReads = false;
    QVERIFY(click(l->setActiveButton));
    for (int attempt = 0; attempt < 100 && active(*l) != kTarget; ++attempt) QTest::qWait(10);
    QCOMPARE(active(*l), kTarget);
}

void ExploreCheckoutTest::unreadableLocalInventoryNeverReachesConfirm_data() {
    QTest::addColumn<QString>("shape");
    for (const char* shape : {"missingField", "nullField", "stringField", "objectField", "numberField",
                              "falseField", "elementNull", "elementString", "elementNoPath",
                              "elementPathNumber", "elementPathEmpty", "otherSchema", "noSchema",
                              "emptyObject", "topLevelList", "lengthOnlyObject", "arrayLikeObject",
                              "keyedObject"}) {
        QTest::newRow(shape) << QString::fromLatin1(shape);
    }
}

// The pre-check must know the local inventory.  An answer it cannot read is a
// failure, never "nothing is local".
void ExploreCheckoutTest::unreadableLocalInventoryNeverReachesConfirm() {
    QFETCH(QString, shape);
    FakeExploreCli cli;
    auto l = load(&cli);
    inspect(*l, kTarget);
    cli.workspacesShape = shape;
    QVERIFY(click(l->checkoutButton));
    QVERIFY2(waitForPhase(*l, QStringLiteral("failed")),
             qPrintable(QStringLiteral("phase is '%1'").arg(phase(*l))));
    settle();
    QCOMPARE(l->controller->property("errorCode").toString(), QStringLiteral("LOCAL_INVENTORY_UNREADABLE"));
    QCOMPARE(l->confirmDialog->property("visible").toBool(), false);
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
    QCOMPARE(active(*l), QString());
    auto* status = l->shell->findChild<QObject*>(QStringLiteral("exploreCheckoutStatus"));
    QCOMPARE(status->property("severity").toString(), QStringLiteral("danger"));
    QVERIFY(status->property("message").toString().contains(QStringLiteral("did not complete")));

    // Once the inventory is readable again the same request goes through.
    cli.workspacesShape = QStringLiteral("ok");
    QVERIFY(click(l->checkoutButton));
    QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
}

void ExploreCheckoutTest::readableInventoryDecidesWhetherCheckoutIsOffered() {
    // A valid, empty inventory is a real answer: nothing is local, confirm is reached.
    {
        FakeExploreCli cli;
        cli.local.clear();
        auto l = load(&cli);
        inspect(*l, kTarget);
        QVERIFY(click(l->checkoutButton));
        QVERIFY(waitForPhase(*l, QStringLiteral("confirm")));
        QCOMPARE(l->controller->property("errorCode").toString(), QString());
        QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
    }
    // A valid inventory that lists the Version is never read as "not local".
    {
        FakeExploreCli cli;
        auto l = load(&cli);
        inspect(*l, kTarget);
        cli.local = QStringList{kLocal, kParent, kTarget};
        QVERIFY(click(l->checkoutButton));
        QVERIFY(waitForPhase(*l, QStringLiteral("failed")));
        QCOMPARE(l->controller->property("errorCode").toString(), QStringLiteral("ALREADY_LOCAL"));
        QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
        settle();
        QCOMPARE(l->checkoutButton->property("visible").toBool(), false);
    }
}

void ExploreCheckoutTest::cliInventoryTextIsJudgedAfterTheBridgeConversion_data() {
    QTest::addColumn<QByteArray>("json");
    QTest::addColumn<QString>("expectedPhase");
    QTest::addColumn<QString>("expectedError");
    // Shape of `isvn workspaces --json` with nothing checked out.
    QTest::newRow("cliEmpty")
        << QByteArray(R"({"schema_version":"workspaces/v1","project":{"name":"project"},"workspaces":[],"issues":[]})")
        << QStringLiteral("confirm") << QString();
    // Shape of `isvn workspaces --json` with the Version checked out.
    QTest::newRow("cliListsTarget")
        << QByteArray(R"({"schema_version":"workspaces/v1","project":{"name":"project"},)"
                      R"("workspaces":[{"workspace_id":"ws-1","phase":"P1-trial","block":"cpu","stage":"200-LOGIC","version":"v2",)"
                      R"("repository_path":"P1-trial/cpu/200-LOGIC/v2","subdir":"P1-trial/cpu/200-LOGIC/v2",)"
                      R"("working_copy":"/demo/learner-workspace/P1-trial/cpu/200-LOGIC/v2",)"
                      R"("counts":{"changed":0,"conflict":0,"local":0},"health":"ok","issues":[]}],"issues":[]})")
        << QStringLiteral("failed") << QStringLiteral("ALREADY_LOCAL");
    QTest::newRow("lengthOnlyObject")
        << QByteArray(R"({"schema_version":"workspaces/v1","workspaces":{"length":0}})")
        << QStringLiteral("failed") << QStringLiteral("LOCAL_INVENTORY_UNREADABLE");
    QTest::newRow("arrayLikeObject")
        << QByteArray(R"({"schema_version":"workspaces/v1","workspaces":{"length":1,"0":{"repository_path":"P1-trial/cpu/200-LOGIC/v1"}}})")
        << QStringLiteral("failed") << QStringLiteral("LOCAL_INVENTORY_UNREADABLE");
}

// CLI output reaches QML as converted by the bridge; a JSON array there is a
// Qt sequence, not a plain JavaScript array.  Judge the real conversion, not a
// hand-built value.
void ExploreCheckoutTest::cliInventoryTextIsJudgedAfterTheBridgeConversion() {
    QFETCH(QByteArray, json);
    QFETCH(QString, expectedPhase);
    QFETCH(QString, expectedError);
    FakeExploreCli cli;
    cli.local.clear();
    auto l = load(&cli);
    inspect(*l, kTarget);
    cli.workspacesJson = json;
    QVERIFY(click(l->checkoutButton));
    QVERIFY2(waitForPhase(*l, expectedPhase),
             qPrintable(QStringLiteral("phase is '%1'").arg(phase(*l))));
    QCOMPARE(l->controller->property("errorCode").toString(), expectedError);
    QCOMPARE(cli.count(QStringLiteral("checkout")), 0);
    QCOMPARE(active(*l), QString());
}

void ExploreCheckoutTest::embeddedBodyReceivesTheProjectPath() {
    FakeExploreCli cli;
    auto l = load(&cli);
    // Without it the body's lineage query passes an empty --project, which the CLI rejects.
    QCOMPARE(l->body->property("projectPath").toString(), QString());
    l->shell->setProperty("projectPath", QStringLiteral("/demo/project"));
    settle();
    QCOMPARE(l->body->property("projectPath").toString(), QStringLiteral("/demo/project"));
    // Query the declared parent of the inspected Version, as the user would.
    inspect(*l, kTarget);
    cli.legacy.clear();
    QVERIFY(QMetaObject::invokeMethod(l->body, "runLineageQuery"));
    settle();
    QVERIFY(!cli.legacy.isEmpty());
    const QStringList query = cli.legacy.constLast();
    QCOMPARE(query.value(0), QStringLiteral("provenance"));
    const int project_flag = query.indexOf(QStringLiteral("--project"));
    QVERIFY(project_flag >= 0);
    QCOMPARE(query.value(project_flag + 1), QStringLiteral("/demo/project"));
}

void ExploreCheckoutTest::controllerSourceHasNoHiddenSideEffects() {
    const QString controller = source(QStringLiteral("qml/shell/ExploreCheckoutController.qml"));
    QVERIFY(!controller.isEmpty());
    // The only mutation is the one checkout of the requested Version.
    QCOMPARE(controller.count(QStringLiteral("beginMutation(")), 1);
    QVERIFY(controller.contains(QStringLiteral("[\"checkout\", targetVersion]")));
    for (const char* forbidden : {"checkout-ancestor", "\"create\"", "\"ancestry\"", "\"reconcile\"",
                                  "\"lineage\"", "selectActive", "revalidateAndSelect",
                                  "setActiveVersion", "runJson"}) {
        QVERIFY2(!controller.contains(QLatin1String(forbidden)), forbidden);
    }
    const QString page = source(QStringLiteral("qml/shell/ExplorePage.qml"));
    QVERIFY(!page.isEmpty());
    for (const char* forbidden : {"checkout-ancestor", "\"checkout\"", "runJson", "selectActive("}) {
        QVERIFY2(!page.contains(QLatin1String(forbidden)), forbidden);
    }
    // Checkout is reached only through the confirmation dialog.
    QCOMPARE(page.count(QStringLiteral("checkoutController.confirm()")), 1);
    QVERIFY(page.contains(QStringLiteral("confirmAction: function() { root.checkoutController.confirm() }")));
}

void ExploreCheckoutTest::shellShowsSuppliedBuildIdentityOnly() {
    FakeExploreCli cli;
    auto l = load(&cli);
    const QString unavailable =
        l->shell->property("shellIdentity").toMap().value(QStringLiteral("buildIdentity")).toString();
    QVERIFY2(unavailable.contains(QStringLiteral("unavailable")), qPrintable(unavailable));
    l->shell->setProperty("buildInfo",
                          QVariantMap{{QStringLiteral("buildIdentity"),
                                       QStringLiteral("isvn-gui 9.9.9 · abc1234-dirty · Debug")}});
    settle();
    QCOMPARE(l->shell->property("shellIdentity").toMap().value(QStringLiteral("buildIdentity")).toString(),
             QStringLiteral("isvn-gui 9.9.9 · abc1234-dirty · Debug"));
    QVERIFY(!source(QStringLiteral("qml/shell/ProductShell.qml")).contains(QStringLiteral("isvn GUI source")));
    QVERIFY(!source(QStringLiteral("src/main.cpp")).contains(QStringLiteral("isvn GUI source")));
}

QTEST_MAIN(ExploreCheckoutTest)
#include "test_explore_checkout.moc"
