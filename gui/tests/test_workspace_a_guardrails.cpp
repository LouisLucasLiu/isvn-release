#include <QFile>
#include <QString>
#include <QTest>

class WorkspaceAGuardrailsTest : public QObject {
    Q_OBJECT

private slots:
    void workspaceAUsesReviewedCliOnly();
    void selectionDoesNotCheckoutContent();
    void comparisonDoesNotAddBackendCommand();
    void renewStatusUsesReviewedCliOnly();
    void stageLegendAlwaysShowsStablePalette();
    void footerNoVersionTextIsConsistent();
};

namespace {

QString readWorkspaceA() {
    QFile file(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
               QStringLiteral("/qml/workspaceA/WorkspaceA.qml"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString readProjectVersionTree() {
    QFile file(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
               QStringLiteral("/qml/workspaceA/ProjectVersionTree.qml"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString readMain() {
    QFile file(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
               QStringLiteral("/qml/Main.qml"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString functionBody(const QString& source, const QString& function_name) {
    const qsizetype start = source.indexOf(QStringLiteral("function ") + function_name);
    if (start < 0) return {};
    const qsizetype next = source.indexOf(QStringLiteral("\n    function "), start + 1);
    return next < 0 ? source.mid(start) : source.mid(start, next - start);
}

}  // namespace

void WorkspaceAGuardrailsTest::workspaceAUsesReviewedCliOnly() {
    const QString source = readWorkspaceA();
    QVERIFY(!source.isEmpty());
    QVERIFY(source.contains(QStringLiteral("branchesRunner.runJson([\"branches\"]")));
    QVERIFY(source.contains(QStringLiteral("statusRunner.runJson([\"status\", selectedVersion])")));
    QVERIFY(source.contains(QStringLiteral("statusRunner.runJson([\"status\", root.selectedVersion])")));
    QVERIFY(source.contains(QStringLiteral("ancestryRunner.runJson([\"ancestry\"")));
    QVERIFY(source.contains(QStringLiteral("\"checkout-ancestor\", selectedVersion, \"--depth\"")));
    QVERIFY(source.contains(QStringLiteral("\"edit\", root.selectedFile, \"--in\", root.selectedVersion")));
    QVERIFY(source.contains(QStringLiteral("\"create\", newVersion, \"--parent\", parent")));
    QVERIFY(source.contains(QStringLiteral("\"checkin\", root.selectedFile, \"--to\", root.selectedVersion")));
    QVERIFY(!source.contains(QStringLiteral("queryCopyFrom")));
    QVERIFY(!source.contains(QStringLiteral("copy-from")));
}

void WorkspaceAGuardrailsTest::selectionDoesNotCheckoutContent() {
    const QString focus_body = functionBody(readWorkspaceA(), QStringLiteral("focusVersion"));
    QVERIFY(!focus_body.isEmpty());
    // Selecting a version refreshes its own file status only; the version graph is
    // loaded by an explicit Refresh graph.
    QVERIFY(focus_body.contains(QStringLiteral("refreshFocusedStatus(false)")));
    QVERIFY(!focus_body.contains(QStringLiteral("refreshFocusedStatus(true)")));
    QVERIFY(!focus_body.contains(QStringLiteral("ancestryRunner")));
    QVERIFY(!focus_body.contains(QStringLiteral("refreshGraph")));
    QVERIFY(!focus_body.contains(QStringLiteral("checkout")));
    QVERIFY(!focus_body.contains(QStringLiteral("checkout-ancestor")));
}

void WorkspaceAGuardrailsTest::comparisonDoesNotAddBackendCommand() {
    const QString source = readWorkspaceA();
    QVERIFY(!source.isEmpty());
    const QString compare_body = functionBody(source, QStringLiteral("runComparison"));
    QVERIFY(!source.contains(QStringLiteral("[\"compare\"")));
    QVERIFY(!compare_body.contains(QStringLiteral("runJson")));
    QVERIFY(compare_body.contains(QStringLiteral("明選版本")));
    QVERIFY(compare_body.contains(QStringLiteral("未自動推斷關係")));
}

void WorkspaceAGuardrailsTest::renewStatusUsesReviewedCliOnly() {
    const QString source = readWorkspaceA();
    QVERIFY(!source.isEmpty());
    const QString body = functionBody(source, QStringLiteral("loadRenewStatus"));
    QVERIFY(body.contains(QStringLiteral("[\"renew-status\", selectedVersion]")));
    QVERIFY(source.contains(QStringLiteral("Retention status")));
    QVERIFY(source.contains(QStringLiteral("保留狀態")));
    QVERIFY(source.contains(QStringLiteral("Shows whether files in this version are retained")));
    QVERIFY(!source.contains(QStringLiteral("last_access_of")));
    QVERIFY(!source.contains(QStringLiteral("meta.db")));
    QVERIFY(!source.contains(QStringLiteral("recent_renews")));
    QVERIFY(!source.contains(QStringLiteral("shared object")));
}

void WorkspaceAGuardrailsTest::stageLegendAlwaysShowsStablePalette() {
    const QString source = readProjectVersionTree();
    QVERIFY(!source.isEmpty());
    const QString body = functionBody(source, QStringLiteral("presentStages"));
    QVERIFY(!body.isEmpty());
    // Must always return the full stable set regardless of how many branches are present.
    // The stable array must be defined unconditionally at the start of the function.
    QVERIFY(body.contains(QStringLiteral("\"RTL\"")));
    QVERIFY(body.contains(QStringLiteral("\"SYN\"")));
    QVERIFY(body.contains(QStringLiteral("\"CTS\"")));
    QVERIFY(body.contains(QStringLiteral("\"ROUTE\"")));
    QVERIFY(body.contains(QStringLiteral("\"SIGNOFF\"")));
    QVERIFY(body.contains(QStringLiteral("\"OTHER\"")));
    // Must NOT use a conditional that skips the stable set when stages exist.
    QVERIFY(!body.contains(QStringLiteral("stages.length > 0 ? stages :")));
    // StageLegend in the tree view must be bound to presentStages().
    QVERIFY(source.contains(QStringLiteral("stages: root.presentStages()")));
}

void WorkspaceAGuardrailsTest::footerNoVersionTextIsConsistent() {
    // displayVersion() must use the same i18n wording as WorkspaceA/B headers.
    const QString source = readMain();
    QVERIFY(!source.isEmpty());
    // "尚未選擇版本" (correct) must be used — not "尚未選取版本" (inconsistent variant).
    QVERIFY(source.contains(QStringLiteral("\"尚未選擇版本\"")));
    QVERIFY(!source.contains(QStringLiteral("\"尚未選取版本\"")));
    // The footer label must use displayVersion() not a hardcoded string.
    QVERIFY(source.contains(QStringLiteral("text: root.displayVersion()")));
}

QTEST_MAIN(WorkspaceAGuardrailsTest)

#include "test_workspace_a_guardrails.moc"
