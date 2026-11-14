// test_work_v2_checkin_clicks — GUI orchestration test. Loads the real
// CheckinBatchController with a programmable fake CLI + a temp-file
// WorkSessionStore and verifies: Add to Batch sends EXACTLY the checked selectors
// (never all-eligible), backend concrete expansion is authoritative, item removal
// re-previews, target switch requires Discard/Cancel and never auto-retargets, a
// blocking conflict cannot reach Confirm, the required message + reserved trailer
// are enforced, Confirm emits exactly one `checkin-batch apply` (never a legacy
// `checkin`, never N applies, never a double activation), Success shows one shared
// revision and clears the Batch, Failed retains the Batch and invalidates the plan,
// Outcome Unknown offers Verify (not Retry), Manual Review stays blocked, stale/
// generation/target-mismatched responses are discarded, and reason codes are
// localized EN/zh_TW/zh_CN.
#include "backend/WorkSessionStore.h"

#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#include <functional>
#include <memory>

using isvn::gui::WorkSessionStore;

class FakeI18n : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString language MEMBER language)
public:
    QString language = QStringLiteral("en");
    Q_INVOKABLE QString text(const QString& en, const QString& tw, const QString& cn) {
        if (language == QStringLiteral("zh_TW")) return tw;
        if (language == QStringLiteral("zh_CN")) return cn;
        return en;
    }
};

class FakeCheckinCli : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    bool running() const { return false; }
    QVector<QStringList> calls;

    // Programmable behavior.
    bool recoveryRequired = false;
    QString conflictPath;              // a selector that previews as CONFLICT_BLOCKED
    QString previewError;              // a bounded backend Preview failure
    QString applyError;                // if set, the next apply fails once with this code
    QString applyStatus = QStringLiteral("committed");  // committed | failed
    int applyRevision = 1305;
    QString verifyResult = QStringLiteral("verified_committed");
    bool defer = false;                // hold the next response until flush()
    std::function<void()> deferred;

    void flush() { if (deferred) { auto f = deferred; deferred = nullptr; f(); } }

    static QStringList pathsOf(const QStringList& args) {
        QStringList paths;
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == QStringLiteral("--path")) paths << args[i + 1];
        return paths;
    }

    Q_INVOKABLE void runJson(const QVariant& arguments) {
        QStringList args;
        for (const QVariant& v : arguments.toList()) args.push_back(v.toString());
        calls.push_back(args);

        auto emitOk = [this](const QVariantMap& m) {
            if (defer) { deferred = [this, m]() { emit commandSucceeded(m); }; }
            else emit commandSucceeded(m);
        };
        auto emitErr = [this](const QString& code, bool recovery) {
            QVariantMap env{{QStringLiteral("schema_version"), QStringLiteral("work-error/v1")},
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("operation_id"), QStringLiteral("op-checkin-batch-1")},
                            {QStringLiteral("error"),
                             QVariantMap{{QStringLiteral("code"), code},
                                         {QStringLiteral("retryable"),
                                          code == QStringLiteral("PLAN_STALE")},
                                         {QStringLiteral("recovery_required"), recovery}}}};
            auto fire = [this, env, code]() {
                emit commandFailedJson(env);
                emit commandFailed(QStringLiteral("failed: ") + code);
            };
            if (defer) deferred = fire; else fire();
        };

        if (args.contains(QStringLiteral("work-recovery")) &&
            args.contains(QStringLiteral("status"))) {
            emitOk(QVariantMap{{QStringLiteral("schema_version"), QStringLiteral("work-recovery/v1")},
                               {QStringLiteral("recovery_required"), recoveryRequired},
                               {QStringLiteral("operations"), QVariantList{}}});
            return;
        }
        if (args.contains(QStringLiteral("work-recovery")) &&
            args.contains(QStringLiteral("verify"))) {
            if (verifyResult == QStringLiteral("manual_review_required")) {
                emitOk(QVariantMap{{QStringLiteral("status"), QStringLiteral("manual_review_required")},
                                   {QStringLiteral("revision"), QVariant()},
                                   {QStringLiteral("reason_code"), QStringLiteral("REMOTE_STATE_DIVERGED")},
                                   {QStringLiteral("manual_review_required"), true},
                                   {QStringLiteral("recovery_required"), true}});
            } else {
                emitOk(QVariantMap{{QStringLiteral("status"), QStringLiteral("verified_committed")},
                                   {QStringLiteral("revision"), 1310},
                                   {QStringLiteral("recovery_required"), false},
                                   {QStringLiteral("manual_review_required"), false}});
            }
            return;
        }
        if (args.contains(QStringLiteral("preview"))) {
            if (!previewError.isEmpty()) {
                const QString code = previewError;
                previewError.clear();
                emitErr(code, false);
                return;
            }
            const QStringList paths = pathsOf(args);
            QVariantList items;
            bool conflict = false;
            for (const QString& p : paths) {
                QVariantMap it{{QStringLiteral("path"), p}, {QStringLiteral("kind"), QStringLiteral("file")},
                               {QStringLiteral("source_kind"), QStringLiteral("LOCAL")}};
                if (p == conflictPath) {
                    it.insert(QStringLiteral("change_kind"), QString());
                    it.insert(QStringLiteral("allowed"), false);
                    it.insert(QStringLiteral("reason_code"), QStringLiteral("CONFLICT_BLOCKED"));
                    conflict = true;
                } else {
                    it.insert(QStringLiteral("change_kind"), QStringLiteral("add"));
                    it.insert(QStringLiteral("allowed"), true);
                    it.insert(QStringLiteral("reason_code"), QStringLiteral("OK"));
                }
                items.push_back(it);
            }
            emitOk(QVariantMap{
                {QStringLiteral("schema"), QStringLiteral("checkin-batch/v1")},
                {QStringLiteral("items"), items},
                {QStringLiteral("supporting_repository_changes"), QVariantList{}},
                {QStringLiteral("can_apply"), !paths.isEmpty() && !conflict},
                {QStringLiteral("plan_hash"), QStringLiteral("H") + QString::number(paths.size())}});
            return;
        }
        if (args.contains(QStringLiteral("apply"))) {
            if (!applyError.isEmpty()) {
                const QString code = applyError;
                applyError.clear();
                emitErr(code, code == QStringLiteral("RECOVERY_REQUIRED"));
                return;
            }
            if (applyStatus == QStringLiteral("failed")) {
                emitOk(QVariantMap{{QStringLiteral("schema"), QStringLiteral("checkin-batch-result/v1")},
                                   {QStringLiteral("operation_id"), QStringLiteral("op-checkin-batch-1")},
                                   {QStringLiteral("status"), QStringLiteral("failed")},
                                   {QStringLiteral("revision"), QVariant()}});
                return;
            }
            emitOk(QVariantMap{{QStringLiteral("schema"), QStringLiteral("checkin-batch-result/v1")},
                               {QStringLiteral("operation_id"), QStringLiteral("op-checkin-batch-1")},
                               {QStringLiteral("status"), QStringLiteral("committed")},
                               {QStringLiteral("revision"), applyRevision}});
            return;
        }
        emitOk(QVariantMap{});
    }

signals:
    void runningChanged();
    void commandSucceeded(const QVariant& json);
    void commandFailed(const QString& message);
    void commandFailedJson(const QVariant& json);
};

class WorkV2CheckinClicksTest : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    int slot_ = 0;
    struct Loaded {
        std::unique_ptr<QQmlEngine> engine;
        QObject* controller = nullptr;
    };
    std::unique_ptr<FakeI18n> i18n_;

    std::unique_ptr<Loaded> load(FakeCheckinCli* cli, WorkSessionStore* store) {
        auto l = std::make_unique<Loaded>();
        l->engine = std::make_unique<QQmlEngine>();
        l->engine->addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        QQmlComponent component(
            l->engine.get(),
            QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                QStringLiteral("/qml/work/CheckinBatchController.qml")));
        QObject* obj = component.create();
        if (obj == nullptr) qFatal("%s", qPrintable(component.errorString()));
        obj->setProperty("isvnCli", QVariant::fromValue(static_cast<QObject*>(cli)));
        obj->setProperty("sessionStore", QVariant::fromValue(static_cast<QObject*>(store)));
        if (!i18n_) i18n_ = std::make_unique<FakeI18n>();
        obj->setProperty("i18n", QVariant::fromValue(static_cast<QObject*>(i18n_.get())));
        obj->setProperty("activeVersion", QStringLiteral("P/B/RTL/v1"));
        l->controller = obj;
        return l;
    }
    WorkSessionStore* store() {
        auto* s = new WorkSessionStore(this);
        s->setStorageFile(dir_.filePath(QStringLiteral("c%1.ini").arg(slot_++)));
        s->setProjectKey(QStringLiteral("/p"));
        return s;
    }
    static QString phase(QObject* c) { return c->property("phase").toString(); }
    static void setSel(QObject* c, const QStringList& paths) {
        QVariantList v;
        for (const QString& p : paths) v.push_back(p);
        QMetaObject::invokeMethod(c, "setSelection", Q_ARG(QVariant, QVariant(v)));
    }
    static void invoke(QObject* c, const char* m, const QVariant& a = QVariant()) {
        if (a.isValid()) QMetaObject::invokeMethod(c, m, Q_ARG(QVariant, a));
        else QMetaObject::invokeMethod(c, m);
        QCoreApplication::processEvents();
    }
    static int applyCount(const QVector<QStringList>& calls) {
        int n = 0;
        for (const QStringList& c : calls)
            if (c.first() == QStringLiteral("checkin-batch") && c.contains(QStringLiteral("apply")))
                ++n;
        return n;
    }
    static QStringList lastPreview(const QVector<QStringList>& calls) {
        for (int i = calls.size() - 1; i >= 0; --i)
            if (calls[i].contains(QStringLiteral("preview"))) return calls[i];
        return {};
    }

private slots:
    void addToBatchSendsExactlyCheckedSelectors();
    void confirmEmitsOneApplyNeverLegacyCheckin();
    void doubleActivationCannotDuplicateApply();
    void itemRemovalRerunsPreview();
    void targetSwitchRequiresDecisionNeverAutoRetarget();
    void conflictBlocksConfirm();
    void requiredMessageAndReservedTrailer();
    void successOneRevisionClearsBatch();
    void failedRetainsBatchInvalidatesPlan();
    void outcomeUnknownHasVerifyNoRetry();
    void verifyManualReviewBlocks();
    void planStaleReturnsToReview();
    void staleTargetResponseDiscarded();
    void reasonCodesLocalized();
    void boundedPreviewFailureInvalidatesOldReview();
    void incompleteRecoveryQueryBlocksRetry();
    void onlyCheckinBatchCommandsEmitted();
};

void WorkV2CheckinClicksTest::addToBatchSendsExactlyCheckedSelectors() {
    FakeCheckinCli cli; auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v"), QStringLiteral("b.v")});  // c.v is eligible but NOT checked
    invoke(c, "addToBatch");
    QCOMPARE(phase(c), QStringLiteral("review"));
    const QStringList prev = lastPreview(cli.calls);
    QCOMPARE(prev.count(QStringLiteral("--path")), 2);
    QVERIFY(prev.contains(QStringLiteral("a.v")) && prev.contains(QStringLiteral("b.v")));
    QVERIFY(!prev.contains(QStringLiteral("c.v")));   // never the unchecked eligible row
    // Backend concrete expansion is authoritative (2 items from the preview).
    QCOMPARE(c->property("itemCount").toInt(), 2);
    QCOMPARE(c->property("canApply").toBool(), true);
}

void WorkV2CheckinClicksTest::confirmEmitsOneApplyNeverLegacyCheckin() {
    FakeCheckinCli cli; auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    invoke(c, "addToBatch");
    QVERIFY(QMetaObject::invokeMethod(c, "continueToConfirm"));
    QCOMPARE(phase(c), QStringLiteral("confirm"));
    c->setProperty("checkinMessage", QStringLiteral("add a.v"));
    invoke(c, "confirmApply");
    QCOMPARE(phase(c), QStringLiteral("success"));
    QCOMPARE(applyCount(cli.calls), 1);
    for (const QStringList& call : cli.calls)
        QVERIFY(call.first() != QStringLiteral("checkin"));  // never legacy per-file checkin
}

void WorkV2CheckinClicksTest::doubleActivationCannotDuplicateApply() {
    FakeCheckinCli cli; cli.defer = true;  // hold the apply so a second click can race
    auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    cli.defer = false; invoke(c, "addToBatch"); cli.defer = false;
    c->setProperty("checkinMessage", QStringLiteral("m"));
    QMetaObject::invokeMethod(c, "continueToConfirm");
    // Two rapid activations before any response is processed.
    cli.defer = true;
    QMetaObject::invokeMethod(c, "confirmApply");
    QMetaObject::invokeMethod(c, "confirmApply");
    cli.flush();
    QCoreApplication::processEvents();
    QCOMPARE(applyCount(cli.calls), 1);  // exactly one apply despite two clicks
}

void WorkV2CheckinClicksTest::itemRemovalRerunsPreview() {
    FakeCheckinCli cli; auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v"), QStringLiteral("b.v")});
    invoke(c, "addToBatch");
    const int previews = cli.calls.size();
    invoke(c, "removeItem", QVariant(QStringLiteral("b.v")));
    // A new preview ran with only a.v; the old plan is invalidated.
    QVERIFY(cli.calls.size() > previews);
    const QStringList prev = lastPreview(cli.calls);
    QVERIFY(prev.contains(QStringLiteral("a.v")) && !prev.contains(QStringLiteral("b.v")));
    QCOMPARE(c->property("itemCount").toInt(), 1);
}

void WorkV2CheckinClicksTest::targetSwitchRequiresDecisionNeverAutoRetarget() {
    FakeCheckinCli cli; auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    invoke(c, "addToBatch");
    QVariant ret;
    QMetaObject::invokeMethod(c, "requestActiveSwitch", Q_RETURN_ARG(QVariant, ret),
                              Q_ARG(QVariant, QVariant(QStringLiteral("P/B/RTL/v2"))));
    QCOMPARE(ret.toBool(), false);                                  // switch refused
    QCOMPARE(c->property("requiresBatchDecision").toBool(), true);
    QCOMPARE(c->property("activeVersion").toString(), QStringLiteral("P/B/RTL/v1"));  // NOT retargeted
    // Discard Batch then the switch proceeds and the Batch is cleared.
    QVERIFY(QMetaObject::invokeMethod(c, "discardBatchAndSwitch"));
    QCOMPARE(c->property("activeVersion").toString(), QStringLiteral("P/B/RTL/v2"));
    QCOMPARE(c->property("itemCount").toInt(), 0);
}

void WorkV2CheckinClicksTest::conflictBlocksConfirm() {
    FakeCheckinCli cli; cli.conflictPath = QStringLiteral("k.v");
    auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v"), QStringLiteral("k.v")});
    invoke(c, "addToBatch");
    QCOMPARE(c->property("hasBlockingConflict").toBool(), true);
    QCOMPARE(c->property("canApply").toBool(), false);
    QVariant ret;
    QMetaObject::invokeMethod(c, "continueToConfirm", Q_RETURN_ARG(QVariant, ret));
    QCOMPARE(ret.toBool(), false);                       // conflict cannot reach Confirm
    QVERIFY(phase(c) != QStringLiteral("confirm"));
}

void WorkV2CheckinClicksTest::requiredMessageAndReservedTrailer() {
    FakeCheckinCli cli; auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    invoke(c, "addToBatch");
    QMetaObject::invokeMethod(c, "continueToConfirm");
    // Empty message: no apply is emitted (backend-required, enforced client-side too).
    c->setProperty("checkinMessage", QStringLiteral("   "));
    invoke(c, "confirmApply");
    QCOMPARE(applyCount(cli.calls), 0);
    QCOMPARE(c->property("errorCode").toString(), QStringLiteral("MESSAGE_REQUIRED"));
    // Reserved trailer: backend rejects; stays in Confirm for correction.
    cli.applyError = QStringLiteral("RESERVED_OPERATION_TRAILER");
    c->setProperty("checkinMessage", QStringLiteral("hi [isvn-op:FAKE]"));
    invoke(c, "confirmApply");
    QCOMPARE(phase(c), QStringLiteral("confirm"));
    QCOMPARE(c->property("errorCode").toString(), QStringLiteral("RESERVED_OPERATION_TRAILER"));
}

void WorkV2CheckinClicksTest::successOneRevisionClearsBatch() {
    FakeCheckinCli cli; cli.applyRevision = 1408;
    auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v"), QStringLiteral("b.v")});
    invoke(c, "addToBatch");
    QMetaObject::invokeMethod(c, "continueToConfirm");
    c->setProperty("checkinMessage", QStringLiteral("commit two"));
    invoke(c, "confirmApply");
    QCOMPARE(phase(c), QStringLiteral("success"));
    QCOMPARE(c->property("verifiedRevision").toInt(), 1408);   // one shared authoritative revision
    QCOMPARE(c->property("selectedPaths").toList().size(), 0); // committed batch cleared
}

void WorkV2CheckinClicksTest::failedRetainsBatchInvalidatesPlan() {
    FakeCheckinCli cli; cli.applyStatus = QStringLiteral("failed");
    auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    invoke(c, "addToBatch");
    QMetaObject::invokeMethod(c, "continueToConfirm");
    c->setProperty("checkinMessage", QStringLiteral("try"));
    invoke(c, "confirmApply");
    QCOMPARE(phase(c), QStringLiteral("failed"));
    QCOMPARE(c->property("selectedPaths").toList().size(), 1);  // batch retained
    QCOMPARE(c->property("planHash").toString(), QString());    // plan invalidated
}

void WorkV2CheckinClicksTest::outcomeUnknownHasVerifyNoRetry() {
    FakeCheckinCli cli; cli.applyError = QStringLiteral("OUTCOME_UNKNOWN");
    auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    invoke(c, "addToBatch");
    QMetaObject::invokeMethod(c, "continueToConfirm");
    c->setProperty("checkinMessage", QStringLiteral("m"));
    invoke(c, "confirmApply");
    QCOMPARE(phase(c), QStringLiteral("outcomeUnknown"));
    QCOMPARE(c->property("mutationsBlocked").toBool(), true);
    const int before = cli.calls.size();
    invoke(c, "verifyOutcome");
    QVERIFY(cli.calls.size() > before);
    const QStringList v = cli.calls.last();
    QVERIFY(v.contains(QStringLiteral("work-recovery")) && v.contains(QStringLiteral("verify")));
    QVERIFY(v.first() != QStringLiteral("checkin-batch"));   // never a blind re-apply
}

void WorkV2CheckinClicksTest::verifyManualReviewBlocks() {
    FakeCheckinCli cli; cli.applyError = QStringLiteral("OUTCOME_UNKNOWN");
    cli.verifyResult = QStringLiteral("manual_review_required");
    auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    invoke(c, "addToBatch");
    QMetaObject::invokeMethod(c, "continueToConfirm");
    c->setProperty("checkinMessage", QStringLiteral("m"));
    invoke(c, "confirmApply");
    invoke(c, "verifyOutcome");
    QCOMPARE(phase(c), QStringLiteral("manualReview"));
    QCOMPARE(c->property("manualReviewCode").toString(), QStringLiteral("REMOTE_STATE_DIVERGED"));
    QCOMPARE(c->property("mutationsBlocked").toBool(), true);
}

void WorkV2CheckinClicksTest::planStaleReturnsToReview() {
    FakeCheckinCli cli; cli.applyError = QStringLiteral("PLAN_STALE");
    auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    invoke(c, "addToBatch");
    QMetaObject::invokeMethod(c, "continueToConfirm");
    c->setProperty("checkinMessage", QStringLiteral("m"));
    invoke(c, "confirmApply");
    // Apply → PLAN_STALE → controller re-previews → back to Review.
    QCOMPARE(phase(c), QStringLiteral("review"));
    QCOMPARE(c->property("errorCode").toString(), QStringLiteral("PLAN_STALE"));
}

void WorkV2CheckinClicksTest::staleTargetResponseDiscarded() {
    FakeCheckinCli cli; cli.defer = true;
    auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    // addToBatch enqueues recovery-gate (deferred). Deliver it, then defer preview.
    QMetaObject::invokeMethod(c, "addToBatch");
    cli.flush();                              // recovery-gate response → issues preview (deferred)
    QCoreApplication::processEvents();
    // Invalidate the batch/target while the preview response is still in flight.
    QMetaObject::invokeMethod(c, "resetBatch");
    const QString phaseBefore = phase(c);
    cli.flush();                              // late preview response arrives after reset
    QCoreApplication::processEvents();
    // The stale response is discarded: it did not move us into Review.
    QCOMPARE(phase(c), phaseBefore);
    QVERIFY(phase(c) != QStringLiteral("review"));
    QCOMPARE(c->property("itemCount").toInt(), 0);
}

void WorkV2CheckinClicksTest::reasonCodesLocalized() {
    FakeCheckinCli cli; auto l = load(&cli, store()); QObject* c = l->controller;
    const char* codes[] = {"SELECTION_REQUIRED", "EDIT_PROVENANCE_REQUIRED", "CONFLICT_BLOCKED",
                           "OUTCOME_UNKNOWN", "REMOTE_STATE_DIVERGED", "SELECTOR_LIMIT_EXCEEDED",
                           "LOCAL_SCAN_LIMIT_EXCEEDED", "BATCH_ITEM_LIMIT_EXCEEDED",
                           "REMOTE_QUERY_LIMIT_EXCEEDED", "SVN_QUERY_TIMEOUT",
                           "SVN_OUTPUT_LIMIT_EXCEEDED", "SVN_QUERY_FAILED",
                           "RECOVERY_QUERY_INCOMPLETE", "OPERATION_IN_PROGRESS",
                           "VERSION_RECOVERY_REQUIRED",
                           "PROJECT_LEGACY_RECOVERY_REQUIRED", "OPERATION_TARGET_DIVERGED",
                           "OPERATION_NOT_FOUND",
                           "READ_LANE_BUSY", "REQUEST_LIMIT_EXCEEDED",
                           "OPERATION_REGISTRY_CORRUPT"};
    for (const char* code : codes) {
        i18n_->language = QStringLiteral("en");
        QVariant en;
        QMetaObject::invokeMethod(c, "reasonText", Q_RETURN_ARG(QVariant, en),
                                  Q_ARG(QVariant, QVariant(QString::fromLatin1(code))));
        i18n_->language = QStringLiteral("zh_TW");
        QVariant tw;
        QMetaObject::invokeMethod(c, "reasonText", Q_RETURN_ARG(QVariant, tw),
                                  Q_ARG(QVariant, QVariant(QString::fromLatin1(code))));
        i18n_->language = QStringLiteral("zh_CN");
        QVariant cn;
        QMetaObject::invokeMethod(c, "reasonText", Q_RETURN_ARG(QVariant, cn),
                                  Q_ARG(QVariant, QVariant(QString::fromLatin1(code))));
        QVERIFY(en.toString() != QString::fromLatin1(code));      // has EN text
        QVERIFY(en.toString() != tw.toString());                 // distinct zh_TW
        QVERIFY(tw.toString() != cn.toString());                 // distinct zh_CN
    }
}

void WorkV2CheckinClicksTest::boundedPreviewFailureInvalidatesOldReview() {
    FakeCheckinCli cli; auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    invoke(c, "addToBatch");
    QCOMPARE(phase(c), QStringLiteral("review"));
    QVERIFY(c->property("canApply").toBool());

    // A later Preview that exceeds a backend-owned budget is not a partial
    // Review.  The concrete plan and Confirm capability must disappear while
    // the user's explicit selectors remain available for narrowing.
    cli.previewError = QStringLiteral("LOCAL_SCAN_LIMIT_EXCEEDED");
    invoke(c, "removeItem", QVariant(QStringLiteral("missing.v")));
    QCOMPARE(phase(c), QStringLiteral("failed"));
    QCOMPARE(c->property("planHash").toString(), QString());
    QVERIFY(!c->property("canApply").toBool());
    QCOMPARE(c->property("selectedPaths").toList().size(), 1);
}

void WorkV2CheckinClicksTest::incompleteRecoveryQueryBlocksRetry() {
    FakeCheckinCli cli; cli.applyError = QStringLiteral("RECOVERY_QUERY_INCOMPLETE");
    auto l = load(&cli, store()); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    invoke(c, "addToBatch");
    QMetaObject::invokeMethod(c, "continueToConfirm");
    c->setProperty("checkinMessage", QStringLiteral("m"));
    invoke(c, "confirmApply");
    QCOMPARE(phase(c), QStringLiteral("manualReview"));
    QCOMPARE(c->property("manualReviewCode").toString(),
             QStringLiteral("RECOVERY_QUERY_INCOMPLETE"));
    QCOMPARE(c->property("mutationsBlocked").toBool(), true);
    QVERIFY(applyCount(cli.calls) == 1);  // never a blind retry
}

void WorkV2CheckinClicksTest::onlyCheckinBatchCommandsEmitted() {
    FakeCheckinCli cli; auto* s = store(); auto l = load(&cli, s); QObject* c = l->controller;
    setSel(c, {QStringLiteral("a.v")});
    invoke(c, "addToBatch");
    QMetaObject::invokeMethod(c, "continueToConfirm");
    c->setProperty("checkinMessage", QStringLiteral("commit"));
    invoke(c, "confirmApply");
    // Only checkin-batch / work-recovery are ever emitted (never legacy checkin,
    // never import, never remove-batch, never N sequential per-file checkins).
    for (const QStringList& call : cli.calls) {
        const QString cmd = call.first();
        QVERIFY(cmd == QStringLiteral("checkin-batch") || cmd == QStringLiteral("work-recovery"));
        QVERIFY(!call.contains(QStringLiteral("checkin")));  // the legacy per-file command token
        QVERIFY(!call.contains(QStringLiteral("import")));
    }
    // Activity Log records only safe, allowlisted fields (no raw command).
    QVERIFY(s->recentActivity().size() >= 1);
    for (const QVariant& rec : s->recentActivity()) {
        const QVariantMap m = rec.toMap();
        QVERIFY(!m.contains(QStringLiteral("rawCommand")));
        QVERIFY(m.contains(QStringLiteral("operationId")));
    }
}

QTEST_MAIN(WorkV2CheckinClicksTest)

#include "test_work_v2_checkin_clicks.moc"
