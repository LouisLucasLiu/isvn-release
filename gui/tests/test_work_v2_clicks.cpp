// test_work_v2_clicks — offscreen interaction test for the Work shell. It
// loads the real ProductShell.qml with its internal typed mock state and
// exercises the state machine and guards. No test spawns a real
// isvn process or mutates a filesystem/repository.
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QVariant>

#include <memory>

class WorkV2ClicksTest : public QObject {
    Q_OBJECT

private slots:
    void checkinFlowTransitionsAndGuards();
    void failedStateRetainsBatch();
    void outcomeUnknownHasVerifyButNoRetry();
    void importDefaultsOffAndManagedIsBlocked();
    void activeSwitchGuards();
    void createCheckoutFailureKeepsPreviousActive();
    void reviewButtonWiringDrivesState();

private:
    struct Loaded {
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem* root = nullptr;
        QObject* workState = nullptr;
    };

    std::unique_ptr<Loaded> loadShell(int width = 1600, int height = 900) {
        auto l = std::make_unique<Loaded>();
        l->engine = std::make_unique<QQmlEngine>();
        l->window = std::make_unique<QQuickWindow>();
        l->engine->addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        QQmlComponent component(l->engine.get(),
                                QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                    QStringLiteral("/qml/shell/ProductShell.qml")));
        QObject* object = component.create();
        if (object == nullptr)
            qFatal("%s", qPrintable(component.errorString()));
        l->root = qobject_cast<QQuickItem*>(object);
        if (l->root == nullptr)
            qFatal("ProductShell root is not a QQuickItem");
        l->window->resize(width, height);
        l->root->setParentItem(l->window->contentItem());
        l->root->setSize(QSizeF(width, height));
        l->window->show();
        if (!QTest::qWaitForWindowExposed(l->window.get()))
            qFatal("window not exposed");
        QCoreApplication::processEvents();
        l->workState = l->root->findChild<QObject*>(QStringLiteral("workState"));
        if (l->workState == nullptr)
            qFatal("workState not found");
        return l;
    }

    static void call(QObject* target, const char* method) {
        QVERIFY(QMetaObject::invokeMethod(target, method));
        QCoreApplication::processEvents();
    }

    static QVariant callRet(QObject* target, const char* method, const QVariant& arg = QVariant()) {
        QVariant ret;
        const bool ok = arg.isValid()
            ? QMetaObject::invokeMethod(target, method, Q_RETURN_ARG(QVariant, ret),
                                        Q_ARG(QVariant, arg))
            : QMetaObject::invokeMethod(target, method, Q_RETURN_ARG(QVariant, ret));
        if (!ok)
            qFatal("invokeMethod failed: %s", method);
        QCoreApplication::processEvents();
        return ret;
    }

    static void loadState(QObject* state, const QString& id) {
        QVERIFY(QMetaObject::invokeMethod(state, "loadState", Q_ARG(QVariant, QVariant(id))));
        QCoreApplication::processEvents();
    }

    static QString stateId(QObject* state) { return state->property("stateId").toString(); }
};

void WorkV2ClicksTest::checkinFlowTransitionsAndGuards() {
    auto l = loadShell();
    QObject* s = l->workState;

    // normal -> review
    loadState(s, QStringLiteral("normal"));
    QCOMPARE(stateId(s), QStringLiteral("normal"));
    call(s, "beginReview");
    QCOMPARE(stateId(s), QStringLiteral("review"));

    // conflict-blocked cannot enter Confirm.
    loadState(s, QStringLiteral("conflictBlocked"));
    QCOMPARE(stateId(s), QStringLiteral("conflictBlocked"));
    QCOMPARE(callRet(s, "continueToConfirm").toBool(), false);
    QCOMPARE(stateId(s), QStringLiteral("conflictBlocked"));

    // valid review -> confirm
    loadState(s, QStringLiteral("review"));
    QCOMPARE(callRet(s, "continueToConfirm").toBool(), true);
    QCOMPARE(stateId(s), QStringLiteral("confirm"));

    // blank message cannot confirm.
    s->setProperty("checkinMessage", QStringLiteral("   "));
    QCOMPARE(callRet(s, "confirmCheckin").toBool(), false);
    QCOMPARE(stateId(s), QStringLiteral("confirm"));
    QCOMPARE(s->property("messageInvalid").toBool(), true);

    // non-empty message -> simulated success (one revision for whole batch).
    s->setProperty("checkinMessage", QStringLiteral("Update synthesis constraints"));
    QCOMPARE(callRet(s, "confirmCheckin").toBool(), true);
    QCOMPARE(stateId(s), QStringLiteral("checkinSuccess"));
    QVERIFY(!s->property("successRevision").toString().isEmpty());
    QCOMPARE(s->property("batchCount").toInt(), 0);
}

void WorkV2ClicksTest::failedStateRetainsBatch() {
    auto l = loadShell();
    QObject* s = l->workState;
    loadState(s, QStringLiteral("checkinFailed"));
    QCOMPARE(stateId(s), QStringLiteral("checkinFailed"));
    QCOMPARE(s->property("batchRetained").toBool(), true);
    QVERIFY(s->property("batchCount").toInt() > 0);
}

void WorkV2ClicksTest::outcomeUnknownHasVerifyButNoRetry() {
    auto l = loadShell();
    QObject* s = l->workState;
    loadState(s, QStringLiteral("outcomeUnknown"));
    QCOMPARE(stateId(s), QStringLiteral("outcomeUnknown"));
    QCOMPARE(s->property("autoRetryEnabled").toBool(), false);

    QQuickItem* verify = l->root->findChild<QQuickItem*>(QStringLiteral("verifyOutcomeButton"));
    QVERIFY(verify != nullptr);
    // No direct Retry control exists in the Outcome Unknown view.
    QVERIFY(l->root->findChild<QQuickItem*>(QStringLiteral("retryButton")) == nullptr);
}

void WorkV2ClicksTest::importDefaultsOffAndManagedIsBlocked() {
    auto l = loadShell();
    QObject* s = l->workState;
    loadState(s, QStringLiteral("importPreview"));
    QCOMPARE(stateId(s), QStringLiteral("importPreview"));
    // The Import Preview view rendered (Apply is a direct child of the view).
    QTRY_VERIFY(l->root->findChild<QQuickItem*>(QStringLiteral("importApplyButton")) != nullptr);

    // Replace Local is default-off. The per-row checkbox binds its checked state
    // to this value, so a default-off value is the authoritative guarantee.
    QCOMPARE(s->property("replaceLocalDefault").toBool(), false);

    // Managed destinations are a hard block and can never become an allowed
    // replacement; existing-LOCAL is the only replaceable destination state.
    QCOMPARE(callRet(s, "canReplace", QVariant(QStringLiteral("managed"))).toBool(), false);
    QCOMPARE(callRet(s, "canReplace", QVariant(QStringLiteral("localExisting"))).toBool(), true);
    QCOMPARE(callRet(s, "canReplace", QVariant(QStringLiteral("absent"))).toBool(), false);
    QCOMPARE(callRet(s, "isImportBlocked", QVariant(QStringLiteral("managed"))).toBool(), true);
}

void WorkV2ClicksTest::activeSwitchGuards() {
    auto l = loadShell();
    QObject* s = l->workState;

    // Dirty local state alone does not block switching (empty batch).
    loadState(s, QStringLiteral("normal"));
    s->setProperty("batchCount", 0);
    QCOMPARE(callRet(s, "requestActiveSwitch", QVariant(QStringLiteral("v0.9"))).toBool(), true);
    QCOMPARE(s->property("activeVersionId").toString(), QStringLiteral("v0.9"));

    // A non-empty batch requires an explicit discard/cancel decision.
    loadState(s, QStringLiteral("normal"));
    QVERIFY(s->property("batchCount").toInt() > 0);
    QCOMPARE(callRet(s, "requestActiveSwitch", QVariant(QStringLiteral("v1.6"))).toBool(), false);
    QCOMPARE(s->property("requiresBatchDecision").toBool(), true);
    QCOMPARE(s->property("activeVersionId").toString(), QStringLiteral("v1.7"));
    // Explicit discard completes the switch.
    QCOMPARE(callRet(s, "discardBatchAndSwitch").toBool(), true);
    QCOMPARE(s->property("activeVersionId").toString(), QStringLiteral("v1.6"));
    QCOMPARE(s->property("batchCount").toInt(), 0);
}

void WorkV2ClicksTest::createCheckoutFailureKeepsPreviousActive() {
    auto l = loadShell();
    QObject* s = l->workState;
    loadState(s, QStringLiteral("normal"));
    QCOMPARE(s->property("activeVersionId").toString(), QStringLiteral("v1.7"));
    call(s, "markCreateCheckoutFailed");
    QCOMPARE(stateId(s), QStringLiteral("createCheckoutFailed"));
    // Created version retained; previous Active unchanged.
    QCOMPARE(s->property("createdVersionId").toString(), QStringLiteral("v1.8"));
    QCOMPARE(s->property("checkoutFailed").toBool(), true);
    QCOMPARE(s->property("activeVersionId").toString(), QStringLiteral("v1.7"));
}

void WorkV2ClicksTest::reviewButtonWiringDrivesState() {
    auto l = loadShell();
    QObject* s = l->workState;
    loadState(s, QStringLiteral("normal"));

    QQuickItem* reviewButton = l->root->findChild<QQuickItem*>(QStringLiteral("reviewCheckinButton"));
    QVERIFY(reviewButton != nullptr);
    QVERIFY(reviewButton->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(reviewButton, "clicked"));
    QCoreApplication::processEvents();
    QCOMPARE(stateId(s), QStringLiteral("review"));
}

QTEST_MAIN(WorkV2ClicksTest)

#include "test_work_v2_clicks.moc"
