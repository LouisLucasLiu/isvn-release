#include <QFile>
#include <QString>
#include <QTest>

class WorkspaceBGuardrailsTest : public QObject {
    Q_OBJECT

private slots:
    void historySourcesStaySeparated();
    void lineageAuditIsReadOnlyAndLineageOnly();
    void reconcileIsReportAndUserDecisionOnly();
    void lineagePanoramaUsesLineageMetadataOnly();
    void sourceCheckNullConsistentNoHintIsNotRun();
    void sourceCheckHasNotRunReasonHelper();
    void fileStateBadgeNormalizesRawSvnStates();
    void noRawSvnTermsInVisibleReasonText();
    void lineageExplorerSurfacesExist();
    void lineageQueryTypesAndAuthorityBadgesExist();
    void lineageExplorerUsesReadOnlyQueryCommands();
    void fileLineagePreservesMultipleCandidates();
    void rootSentinelVersionNodesRemainQueryable();
    void missingCommandStateExists();
    void evidenceLoadsOnlyOnExplicitRefresh();
};

namespace {

QString readWorkspaceB() {
    QFile file(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
               QStringLiteral("/qml/workspaceB/WorkspaceB.qml"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString readFileStateBadge() {
    QFile file(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
               QStringLiteral("/qml/common/FileStateBadge.qml"));
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

void WorkspaceBGuardrailsTest::historySourcesStaySeparated() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    QVERIFY(source.contains(QStringLiteral("ListModel { id: svnLogModel }")));
    QVERIFY(source.contains(QStringLiteral("ListModel { id: lineageAuditModel }")));
    QVERIFY(!source.contains(QStringLiteral("historyModel")));
    QVERIFY(!source.contains(QStringLiteral("unifiedHistory")));
    QVERIFY(source.contains(QStringLiteral("Commit history")));
    QVERIFY(source.contains(QStringLiteral("Provenance log")));
}

void WorkspaceBGuardrailsTest::lineageAuditIsReadOnlyAndLineageOnly() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    const QString audit_body = functionBody(source, QStringLiteral("loadLineageAudit"));
    QVERIFY(audit_body.contains(QStringLiteral("[\"lineage\", \"audit\"]")));
    QVERIFY(!audit_body.contains(QStringLiteral("log")));
    QVERIFY(!audit_body.contains(QStringLiteral("reconcile")));
    QVERIFY(source.contains(QStringLiteral("Provenance log")));
    QVERIFY(source.contains(QStringLiteral("版本來源紀錄")));
    QVERIFY(!audit_body.contains(QStringLiteral("queryCopyFrom")));
    QVERIFY(!audit_body.contains(QStringLiteral("copy-from")));
}

void WorkspaceBGuardrailsTest::lineagePanoramaUsesLineageMetadataOnly() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    const QString audit_body = functionBody(source, QStringLiteral("loadLineageAudit"));
    const QString populate_body = functionBody(source, QStringLiteral("populateLineageAudit"));
    QVERIFY(source.contains(QStringLiteral("ListModel { id: lineagePanoramaModel }")));
    QVERIFY(source.contains(QStringLiteral("Version graph")));
    QVERIFY(audit_body.contains(QStringLiteral("[\"lineage\", \"audit\"]")));
    QVERIFY(populate_body.contains(QStringLiteral("root.addPanoramaNode")));
    QVERIFY(source.contains(QStringLiteral("點節點看資訊")));
    QVERIFY(!source.contains(QStringLiteral("queryCopyFrom")));
    QVERIFY(!source.contains(QStringLiteral("[\"checkout\"")));
    QVERIFY(!source.contains(QStringLiteral("[\"checkout-ancestor\"")));
    QVERIFY(!source.contains(QStringLiteral("pool-stat")));
}

void WorkspaceBGuardrailsTest::reconcileIsReportAndUserDecisionOnly() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    const QString reconcile_body = functionBody(source, QStringLiteral("runReconcileReport"));
    QVERIFY(reconcile_body.contains(QStringLiteral("[\"reconcile\"]")));
    QVERIFY(!source.contains(QStringLiteral("autoResolve")));
    QVERIFY(!source.contains(QStringLiteral("autoApply")));
    QVERIFY(!source.contains(QStringLiteral("自動修正")));
    QVERIFY(!source.contains(QStringLiteral("svn 是否套用")));
    QVERIFY(source.contains(QStringLiteral("You decide the correct source")));
    QVERIFY(source.contains(QStringLiteral("GUI 未變更版本來源紀錄")));
}

void WorkspaceBGuardrailsTest::sourceCheckNullConsistentNoHintIsNotRun() {
    // When the backend returns consistent=null with no copy-from hint, the GUI must
    // show NOT RUN (not REVIEW NEEDED), because there is nothing to compare against.
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    const QString body = functionBody(source, QStringLiteral("normalizedSourceCheckState"));
    QVERIFY(!body.isEmpty());
    // "review_needed" is guarded behind a hint check; "not_run" is the final fallback.
    QVERIFY(body.contains(QStringLiteral("reconcileSvnHint.length")));
    QVERIFY(body.contains(QStringLiteral("\"review_needed\"")));
    QVERIFY(body.contains(QStringLiteral("\"not_run\"")));
    // "not_run" must appear after "review_needed" so it is the unconditional fallback.
    QVERIFY(body.lastIndexOf(QStringLiteral("\"not_run\"")) >
            body.indexOf(QStringLiteral("\"review_needed\"")));
    // Bare "return \"review_needed\"" must NOT be the final statement.
    const qsizetype last_return = body.lastIndexOf(QStringLiteral("return "));
    QVERIFY(body.mid(last_return).startsWith(QStringLiteral("return \"not_run\"")));
}

void WorkspaceBGuardrailsTest::sourceCheckHasNotRunReasonHelper() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    // Function must exist and explain why the check has no result.
    QVERIFY(source.contains(QStringLiteral("function sourceCheckNotRunReason")));
    const QString body = functionBody(source, QStringLiteral("sourceCheckNotRunReason"));
    QVERIFY(!body.isEmpty());
    QVERIFY(body.contains(QStringLiteral("reconcileLineageParent")));
    // Must not surface internal backend terms or raw JSON keys.
    QVERIFY(!body.contains(QStringLiteral("svn_copyfrom_hint")));
    QVERIFY(!body.contains(QStringLiteral("queryCopyFrom")));
    // "copy-from" is an SVN-internal term; must not appear in the user-visible reason text.
    QVERIFY(!body.contains(QStringLiteral("copy-from")));
    // Must give a user-readable explanation ("source" is acceptable).
    QVERIFY(body.contains(QStringLiteral("source")));
}

void WorkspaceBGuardrailsTest::noRawSvnTermsInVisibleReasonText() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    // The i18n reason text shown to users must not include SVN-internal terms.
    // Scan for raw terms inside i18n.text() arguments.
    // "copy-from": SVN internal — user sees "source reference" instead.
    // Note: "reconcile" may appear as a QML variable/signal name (allowed) but must
    // not appear in the English text of an i18n.text() user-visible string.
    QVERIFY(!source.contains(QStringLiteral("\"No copy-from")));
    QVERIFY(!source.contains(QStringLiteral("\"copy-from reference")));
    // "obstructed" must not appear in i18n visible-text arguments.
    QVERIFY(!source.contains(QStringLiteral("i18n.text(\"obstructed\"")));
    // Verify user-facing "Source" messaging is present.
    QVERIFY(source.contains(QStringLiteral("No source reference")));
}

void WorkspaceBGuardrailsTest::fileStateBadgeNormalizesRawSvnStates() {
    const QString source = readFileStateBadge();
    QVERIFY(!source.isEmpty());
    // svnStateLabel() function must exist.
    QVERIFY(source.contains(QStringLiteral("function svnStateLabel")));
    // "obstructed" must map to a friendly label, not be passed through raw.
    QVERIFY(source.contains(QStringLiteral("\"obstructed\"")));
    // The visible Label must use svnStateLabel(), not the raw svnState property.
    QVERIFY(source.contains(QStringLiteral("text: root.svnStateLabel()")));
    // Must NOT pass raw svnState to label text (e.g. "text: root.svnState.length > 0…").
    QVERIFY(!source.contains(QStringLiteral("text: root.svnState.length")));
    // The friendly label for obstructed must be "retained" (same as linkLabel for LINKED).
    const QString body = functionBody(source, QStringLiteral("svnStateLabel"));
    QVERIFY(body.contains(QStringLiteral("obstructed")));
    QVERIFY(body.contains(QStringLiteral("retained")));
}

void WorkspaceBGuardrailsTest::lineageExplorerSurfacesExist() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    QVERIFY(source.contains(QStringLiteral("Lineage & Evidence Explorer")));
    QVERIFY(source.contains(QStringLiteral("lineageContextBar")));
    QVERIFY(source.contains(QStringLiteral("lineageQueryPanel")));
    QVERIFY(source.contains(QStringLiteral("lineageGraphCanvas")));
    QVERIFY(source.contains(QStringLiteral("lineageNodeInspector")));
    QVERIFY(source.contains(QStringLiteral("lineageResultSummaryStrip")));
}

void WorkspaceBGuardrailsTest::lineageQueryTypesAndAuthorityBadgesExist() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    QVERIFY(source.contains(QStringLiteral("Parent")));
    QVERIFY(source.contains(QStringLiteral("Ancestor Chain")));
    QVERIFY(source.contains(QStringLiteral("Root")));
    QVERIFY(source.contains(QStringLiteral("Pedigree")));
    QVERIFY(source.contains(QStringLiteral("Versions")));
    QVERIFY(source.contains(QStringLiteral("Parent Versions")));
    QVERIFY(source.contains(QStringLiteral("Root Versions")));
    QVERIFY(source.contains(QStringLiteral("Declared provenance")));
    QVERIFY(source.contains(QStringLiteral("Declared inventory")));
    QVERIFY(source.contains(QStringLiteral("Not content-verified")));
}

void WorkspaceBGuardrailsTest::lineageExplorerUsesReadOnlyQueryCommands() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    const QString query_body = functionBody(source, QStringLiteral("runLineageQuery"));
    QVERIFY(query_body.contains(QStringLiteral("\"provenance\", \"query\"")));
    QVERIFY(query_body.contains(QStringLiteral("\"file\", \"query\"")));
    QVERIFY(query_body.contains(QStringLiteral("\"--project\"")));
    QVERIFY(query_body.contains(QStringLiteral("\"--version\"")));
    QVERIFY(query_body.contains(QStringLiteral("\"--path\"")));
    QVERIFY(!query_body.contains(QStringLiteral("\"checkout\"")));
    QVERIFY(!query_body.contains(QStringLiteral("\"edit\"")));
    QVERIFY(!query_body.contains(QStringLiteral("\"checkin\"")));
    QVERIFY(!query_body.contains(QStringLiteral("\"gc\"")));
    QVERIFY(!source.contains(QStringLiteral("queryCopyFrom")));
    QVERIFY(!source.contains(QStringLiteral("content_verified = true")));
}

void WorkspaceBGuardrailsTest::fileLineagePreservesMultipleCandidates() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    const QString body = functionBody(source, QStringLiteral("populateFileLineage"));
    QVERIFY(body.contains(QStringLiteral("parent_versions")));
    QVERIFY(body.contains(QStringLiteral("root_versions")));
    QVERIFY(body.contains(QStringLiteral("candidates.length")));
    QVERIFY(body.contains(QStringLiteral("multiple candidate versions")));
    QVERIFY(body.contains(QStringLiteral("for (var c = 0; c < candidates.length; ++c)")));
    QVERIFY(!body.contains(QStringLiteral("candidates[0]")));
}

void WorkspaceBGuardrailsTest::rootSentinelVersionNodesRemainQueryable() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    const QString append_body = functionBody(source, QStringLiteral("appendVersionNode"));
    QVERIFY(append_body.contains(QStringLiteral("versionObject.version === \"ROOT\"")));
    QVERIFY(append_body.contains(QStringLiteral("queryTarget")));
    QVERIFY(append_body.contains(QStringLiteral("\"ROOT\"")));
    QVERIFY(source.contains(QStringLiteral("root.versionQueryTarget = node.queryTarget || node.version")));
    QVERIFY(!source.contains(QStringLiteral("[node.phase, node.block, node.stage, node.version].join(\"/\")")));
}

void WorkspaceBGuardrailsTest::missingCommandStateExists() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    QVERIFY(source.contains(QStringLiteral("populateLineageUnavailable")));
    QVERIFY(source.contains(QStringLiteral("lineageMissingCommandState")));
    QVERIFY(source.contains(QStringLiteral("QUERY_NOT_AVAILABLE")));
    QVERIFY(source.contains(QStringLiteral("not_available")));
}

void WorkspaceBGuardrailsTest::evidenceLoadsOnlyOnExplicitRefresh() {
    const QString source = readWorkspaceB();
    QVERIFY(!source.isEmpty());
    // No automatic evidence load on creation, visibility or selection changes.
    QVERIFY(!source.contains(QStringLiteral("autoLoadIfReady")));
    QVERIFY(!source.contains(QStringLiteral("Component.onCompleted: refreshAllReview")));
    QVERIFY(!source.contains(QStringLiteral("onVisibleChanged: if (visible) refreshAllReview")));
    const QString target_changed = functionBody(source, QStringLiteral("reviewTargetChanged"));
    QVERIFY(!target_changed.isEmpty());
    QVERIFY(!target_changed.contains(QStringLiteral("refreshAllReview")));
    QVERIFY(!target_changed.contains(QStringLiteral("runJson")));
    // refreshAllReview is reachable only from the two Refresh evidence buttons.
    QCOMPARE(source.count(QStringLiteral("root.refreshAllReview()")), 2);
    // Overlapping refreshes are refused and failures stay visible.
    QVERIFY(functionBody(source, QStringLiteral("refreshAllReview")).contains(QStringLiteral("if (refreshAllInProgress)")));
    QCOMPARE(source.count(QStringLiteral("enabled: root.hasVersion() && !root.refreshAllInProgress")), 3);
    QVERIFY(source.contains(QStringLiteral("objectName: \"evidenceStateBanner\"")));
    QCOMPARE(source.count(QStringLiteral("root.evidenceError = message")), 2);
}

QTEST_MAIN(WorkspaceBGuardrailsTest)

#include "test_workspace_b_guardrails.moc"
