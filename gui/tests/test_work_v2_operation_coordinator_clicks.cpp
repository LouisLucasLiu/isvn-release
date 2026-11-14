#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTest>
#include <QVariant>

#include <memory>

class FakeCoordinatorCli : public QObject {
    Q_OBJECT
    Q_PROPERTY(int activeMutations MEMBER activeMutations NOTIFY operationLaneChanged)
    Q_PROPERTY(int activeReads MEMBER activeReads NOTIFY operationLaneChanged)

public:
    struct Call { QString operationId; QString target; QVariant args; bool mutation = false; };
    int activeMutations = 0;
    int activeReads = 0;
    QVector<Call> calls;
    bool rejectNext = false;

    Q_INVOKABLE bool runJsonForOperation(const QString& operationId, const QString& target,
                                         const QVariant& args, const QString&, bool mutation) {
        calls.push_back(Call{operationId, target, args, mutation});
        if (rejectNext) {
            rejectNext = false;
            emit operationFailedJson(operationId, target,
                                     QVariantMap{{QStringLiteral("error"),
                                                  QVariantMap{{QStringLiteral("code"),
                                                               QStringLiteral("AUTHORIZATION_DENIED")},
                                                              {QStringLiteral("recovery_required"), false}}}});
            return false;
        }
        return true;
    }

    void succeed(const QString& operationId, const QString& target, const QVariantMap& json) {
        emit operationSucceeded(operationId, target, json);
    }

signals:
    void operationLaneChanged();
    void operationSucceeded(const QString& operationId, const QString& target, const QVariant& json);
    void operationFailedJson(const QString& operationId, const QString& target, const QVariant& json);
};

class OperationCoordinatorClicksTest : public QObject {
    Q_OBJECT

private:
    static QObject* load(QQmlEngine& engine, FakeCoordinatorCli* cli) {
        engine.addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                              QStringLiteral("/qml/work/OperationCoordinator.qml")));
        QObject* object = component.create();
        if (object == nullptr) qFatal("%s", qPrintable(component.errorString()));
        object->setProperty("isvnCli", QVariant::fromValue(static_cast<QObject*>(cli)));
        return object;
    }

    static QString begin(QObject* coordinator, const QString& kind, const QString& target,
                         bool mutation = true) {
        QVariant returned;
        const bool invoked = QMetaObject::invokeMethod(
            coordinator, mutation ? "beginMutation" : "beginRead", Q_RETURN_ARG(QVariant, returned),
            Q_ARG(QVariant, QVariant(kind)), Q_ARG(QVariant, QVariant(target)),
            Q_ARG(QVariant, QVariant(QVariantList{QStringLiteral("import"),
                                                   mutation ? QStringLiteral("apply")
                                                            : QStringLiteral("preview")})),
            Q_ARG(QVariant, QVariant(QString())), Q_ARG(QVariant, QVariant(QStringLiteral("hash"))));
        if (!invoked) return {};
        return returned.toString();
    }

private slots:
    void completionRemainsPinnedAfterActiveSwitch();
    void draftsAndImmediateFailureStayOnTheirTarget();
    void importControllersCanRunTwoPinnedVersionsWithoutCrossRouting();
    void checkinDraftMovesByTargetWhilePriorApplyContinues();
    void finalRecoveryClearsTheRecoveryState();
};

// A recovery that reached a final result releases the Version again; until then the
// Version needs recovery, while a merely running operation never blocks it.
void OperationCoordinatorClicksTest::finalRecoveryClearsTheRecoveryState() {
    FakeCoordinatorCli cli;
    QQmlEngine engine;
    std::unique_ptr<QObject> coordinator(load(engine, &cli));
    const auto needs = [&coordinator](const QString& version) {
        QVariant value;
        QMetaObject::invokeMethod(coordinator.get(), "needsRecovery", Q_RETURN_ARG(QVariant, value),
                                  Q_ARG(QVariant, QVariant(version)));
        return value.toBool();
    };
    const QString running = begin(coordinator.get(), QStringLiteral("import"), QStringLiteral("P/B/RTL/A"));
    QVERIFY(!running.isEmpty());
    QVERIFY(!needs(QStringLiteral("P/B/RTL/A")));
    const QVariantList operations{QVariantMap{
        {QStringLiteral("operation_id"), QStringLiteral("op-remove-1")},
        {QStringLiteral("operation"), QStringLiteral("remove-batch")},
        {QStringLiteral("target_version"), QStringLiteral("P/B/RTL/A")},
        {QStringLiteral("state"), QStringLiteral("RECOVERY_REQUIRED")}}};
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "recordRecoveryOperations",
                                      Q_ARG(QVariant, QVariant(operations))));
    QVERIFY(needs(QStringLiteral("P/B/RTL/A")));
    QVERIFY(!needs(QStringLiteral("P/B/RTL/B")));
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "resolveRecovery",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("op-remove-1")))));
    QVERIFY(!needs(QStringLiteral("P/B/RTL/A")));
}

void OperationCoordinatorClicksTest::completionRemainsPinnedAfterActiveSwitch() {
    FakeCoordinatorCli cli;
    QQmlEngine engine;
    std::unique_ptr<QObject> coordinator(load(engine, &cli));
    coordinator->setProperty("activeVersion", QStringLiteral("P/B/RTL/A"));
    const QString opA = begin(coordinator.get(), QStringLiteral("import"), QStringLiteral("P/B/RTL/A"));
    QVERIFY(!opA.isEmpty());
    coordinator->setProperty("activeVersion", QStringLiteral("P/B/RTL/B"));
    const QString opB = begin(coordinator.get(), QStringLiteral("checkin"), QStringLiteral("P/B/RTL/B"));
    QVERIFY(!opB.isEmpty());
    QCOMPARE(cli.calls.size(), 2);
    QCOMPARE(cli.calls.at(0).target, QStringLiteral("P/B/RTL/A"));
    QCOMPARE(cli.calls.at(1).target, QStringLiteral("P/B/RTL/B"));

    cli.succeed(opA, QStringLiteral("P/B/RTL/A"),
                QVariantMap{{QStringLiteral("status"), QStringLiteral("imported")},
                            {QStringLiteral("batch_paths"),
                             QVariantList{QStringLiteral("src/a.v")}}});
    QCoreApplication::processEvents();
    QCOMPARE(coordinator->property("activeVersion").toString(), QStringLiteral("P/B/RTL/B"));
    QVariant operation;
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "operationFor", Q_RETURN_ARG(QVariant, operation),
                                      Q_ARG(QVariant, QVariant(opA))));
    QCOMPARE(operation.toMap().value(QStringLiteral("targetVersion")).toString(),
             QStringLiteral("P/B/RTL/A"));
    QCOMPARE(operation.toMap().value(QStringLiteral("state")).toString(), QStringLiteral("success"));
    QVariant draftA;
    QVariant draftB;
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "draftFor", Q_RETURN_ARG(QVariant, draftA),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("P/B/RTL/A")))));
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "draftFor", Q_RETURN_ARG(QVariant, draftB),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("P/B/RTL/B")))));
    QCOMPARE(draftA.toMap().value(QStringLiteral("pendingBatchPaths")).toStringList(),
             QStringList{QStringLiteral("src/a.v")});
    QVERIFY(draftB.toMap().value(QStringLiteral("pendingBatchPaths")).toList().isEmpty());
}

void OperationCoordinatorClicksTest::draftsAndImmediateFailureStayOnTheirTarget() {
    FakeCoordinatorCli cli;
    QQmlEngine engine;
    std::unique_ptr<QObject> coordinator(load(engine, &cli));
    QVariant ignored;
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "updateDraft", Q_ARG(QVariant, QVariant(QStringLiteral("A"))),
                                      Q_ARG(QVariant, QVariant(QVariantMap{{QStringLiteral("checkinMessage"),
                                                                            QStringLiteral("message A")}}))));
    cli.rejectNext = true;
    const QString opC = begin(coordinator.get(), QStringLiteral("import"), QStringLiteral("C"));
    QVERIFY(!opC.isEmpty());
    QCoreApplication::processEvents();
    QVariant failed;
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "operationFor", Q_RETURN_ARG(QVariant, failed),
                                      Q_ARG(QVariant, QVariant(opC))));
    QCOMPARE(failed.toMap().value(QStringLiteral("targetVersion")).toString(), QStringLiteral("C"));
    QCOMPARE(failed.toMap().value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("AUTHORIZATION_DENIED"));
    QVariant draft;
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "draftFor", Q_RETURN_ARG(QVariant, draft),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("A")))));
    QCOMPARE(draft.toMap().value(QStringLiteral("checkinMessage")).toString(), QStringLiteral("message A"));
    Q_UNUSED(ignored);
}

void OperationCoordinatorClicksTest::importControllersCanRunTwoPinnedVersionsWithoutCrossRouting() {
    FakeCoordinatorCli cli;
    QQmlEngine engine;
    std::unique_ptr<QObject> coordinator(load(engine, &cli));
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                          QStringLiteral("/qml/work/ImportController.qml")));
    std::unique_ptr<QObject> controller(component.create());
    if (!controller) qFatal("%s", qPrintable(component.errorString()));
    controller->setProperty("isvnCli", QVariant::fromValue(static_cast<QObject*>(&cli)));
    controller->setProperty("operationCoordinator", QVariant::fromValue(coordinator.get()));
    controller->setProperty("activeVersion", QStringLiteral("A"));

    const QVariantList urls{QStringLiteral("file:///unique-external-source-a")};
    QVariant begun;
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "startDrop", Q_RETURN_ARG(QVariant, begun),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("src"))),
                                      Q_ARG(QVariant, QVariant(urls))));
    QVERIFY(begun.toBool());
    QCOMPARE(cli.calls.size(), 1);
    QCOMPARE(cli.calls.at(0).target, QStringLiteral("A"));
    QVERIFY(!cli.calls.at(0).mutation);
    const QString previewA = cli.calls.at(0).operationId;
    cli.succeed(previewA, QStringLiteral("A"),
                QVariantMap{{QStringLiteral("schema"), QStringLiteral("import/v1")},
                            {QStringLiteral("plan_hash"), QStringLiteral("plan-a")},
                            {QStringLiteral("items"), QVariantList{QVariantMap{{QStringLiteral("destination"), QStringLiteral("src/a.v")},
                                                                                  {QStringLiteral("allowed"), true}}}}});
    QCoreApplication::processEvents();
    QCOMPARE(controller->property("phase").toString(), QStringLiteral("review"));
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "continueToConfirm"));
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "confirmApply"));
    QCOMPARE(cli.calls.size(), 2);
    QVERIFY(cli.calls.at(1).mutation);
    const QString applyA = cli.calls.at(1).operationId;

    // A remains running in the coordinator while the visible target switches.
    // B gets its own Preview/Apply pipeline; neither response is allowed to
    // overwrite whichever Version is currently visible.
    controller->setProperty("activeVersion", QStringLiteral("B"));
    const QVariantList urlsB{QStringLiteral("file:///unique-external-source-b")};
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "startDrop", Q_RETURN_ARG(QVariant, begun),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("rtl"))),
                                      Q_ARG(QVariant, QVariant(urlsB))));
    QVERIFY(begun.toBool());
    QCOMPARE(cli.calls.size(), 3);
    QCOMPARE(cli.calls.at(2).target, QStringLiteral("B"));
    const QString previewB = cli.calls.at(2).operationId;
    cli.succeed(previewB, QStringLiteral("B"),
                QVariantMap{{QStringLiteral("schema"), QStringLiteral("import/v1")},
                            {QStringLiteral("plan_hash"), QStringLiteral("plan-b")},
                            {QStringLiteral("items"), QVariantList{QVariantMap{{QStringLiteral("destination"), QStringLiteral("rtl/b.v")},
                                                                                  {QStringLiteral("allowed"), true}}}}});
    QCoreApplication::processEvents();
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "continueToConfirm"));
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "confirmApply"));
    QCOMPARE(cli.calls.size(), 4);
    QCOMPARE(cli.calls.at(3).target, QStringLiteral("B"));
    QVERIFY(cli.calls.at(3).mutation);

    cli.succeed(applyA, QStringLiteral("A"),
                QVariantMap{{QStringLiteral("schema"), QStringLiteral("import-result/v1")},
                            {QStringLiteral("status"), QStringLiteral("imported")},
                            {QStringLiteral("batch_paths"), QVariantList{QStringLiteral("src/a.v")}}});
    QCoreApplication::processEvents();
    QCOMPARE(controller->property("activeVersion").toString(), QStringLiteral("B"));
    QCOMPARE(controller->property("phase").toString(), QStringLiteral("applying"));
    QVariant draftA;
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "draftFor", Q_RETURN_ARG(QVariant, draftA),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("A")))));
    QCOMPARE(draftA.toMap().value(QStringLiteral("pendingBatchPaths")).toStringList(),
             QStringList{QStringLiteral("src/a.v")});
}

void OperationCoordinatorClicksTest::checkinDraftMovesByTargetWhilePriorApplyContinues() {
    FakeCoordinatorCli cli;
    QQmlEngine engine;
    std::unique_ptr<QObject> coordinator(load(engine, &cli));
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                          QStringLiteral("/qml/work/CheckinBatchController.qml")));
    std::unique_ptr<QObject> controller(component.create());
    if (!controller) qFatal("%s", qPrintable(component.errorString()));
    controller->setProperty("isvnCli", QVariant::fromValue(static_cast<QObject*>(&cli)));
    controller->setProperty("operationCoordinator", QVariant::fromValue(coordinator.get()));
    controller->setProperty("activeVersion", QStringLiteral("A"));

    QVERIFY(QMetaObject::invokeMethod(controller.get(), "setSelection",
                                      Q_ARG(QVariant, QVariant(QVariantList{QStringLiteral("a.v")}))));
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "addToBatch"));
    QCOMPARE(cli.calls.last().target, QStringLiteral("A"));
    const QString gateA = cli.calls.last().operationId;
    cli.succeed(gateA, QStringLiteral("A"), QVariantMap{{QStringLiteral("recovery_required"), false}});
    QCoreApplication::processEvents();
    const QString previewA = cli.calls.last().operationId;
    cli.succeed(previewA, QStringLiteral("A"),
                QVariantMap{{QStringLiteral("schema"), QStringLiteral("checkin-batch/v1")},
                            {QStringLiteral("plan_hash"), QStringLiteral("checkin-a")},
                            {QStringLiteral("can_apply"), true},
                            {QStringLiteral("items"), QVariantList{QVariantMap{{QStringLiteral("path"), QStringLiteral("a.v")},
                                                                                  {QStringLiteral("allowed"), true}}}}});
    QCoreApplication::processEvents();
    QCOMPARE(controller->property("phase").toString(), QStringLiteral("review"));
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "continueToConfirm"));
    controller->setProperty("checkinMessage", QStringLiteral("message A"));
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "confirmApply"));
    const QString applyA = cli.calls.last().operationId;
    QCOMPARE(cli.calls.last().target, QStringLiteral("A"));
    QVERIFY(cli.calls.last().mutation);

    controller->setProperty("activeVersion", QStringLiteral("B"));
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "setSelection",
                                      Q_ARG(QVariant, QVariant(QVariantList{QStringLiteral("b.v")}))));
    QVERIFY(QMetaObject::invokeMethod(controller.get(), "addToBatch"));
    QCOMPARE(cli.calls.last().target, QStringLiteral("B"));
    const QString gateB = cli.calls.last().operationId;
    cli.succeed(gateB, QStringLiteral("B"), QVariantMap{{QStringLiteral("recovery_required"), false}});
    QCoreApplication::processEvents();
    QCOMPARE(cli.calls.last().target, QStringLiteral("B"));

    // Completing A while B is visible cannot overwrite B's current Review.
    cli.succeed(applyA, QStringLiteral("A"),
                QVariantMap{{QStringLiteral("status"), QStringLiteral("committed")},
                            {QStringLiteral("revision"), 42}});
    QCoreApplication::processEvents();
    QCOMPARE(controller->property("activeVersion").toString(), QStringLiteral("B"));
    QVariant draftA;
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "draftFor", Q_RETURN_ARG(QVariant, draftA),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("A")))));
    QCOMPARE(draftA.toMap().value(QStringLiteral("checkinMessage")).toString(), QString());
    QVERIFY(draftA.toMap().value(QStringLiteral("selectedPaths")).toList().isEmpty());
    QVariant draftB;
    QVERIFY(QMetaObject::invokeMethod(coordinator.get(), "draftFor", Q_RETURN_ARG(QVariant, draftB),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("B")))));
    QCOMPARE(draftB.toMap().value(QStringLiteral("selectedPaths")).toStringList(),
             QStringList{QStringLiteral("b.v")});
}

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    OperationCoordinatorClicksTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_work_v2_operation_coordinator_clicks.moc"
