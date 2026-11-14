import QtQuick

// WorkUiState — the Work UI state machine.
//
// It is a pure front-end controller over WorkMockModel. It performs NO backend
// call, NO SVN, NO filesystem mutation. Every transition is local UI state and
// enforces these rules:
//   - one atomic Batch Checkin == one revision (never N sequential revisions)
//   - a non-empty Checkin Message is required to confirm
//   - a blocking conflict blocks the WHOLE batch (cannot reach Confirm)
//   - Success / Failed / Outcome Unknown are distinct (no partial-batch result)
//   - Outcome Unknown disables automatic retry (verify first)
//   - managed import destinations can never become an allowed replacement
//   - startup never auto-checks-out a version and never picks a fallback Active
QtObject {
    id: root

    property var model

    // Deterministic state IDs. loadState() selects any of these.
    readonly property string stateNormal: "normal"
    readonly property string stateReview: "review"
    readonly property string stateConflictBlocked: "conflictBlocked"
    readonly property string stateConfirm: "confirm"
    readonly property string stateCheckinSuccess: "checkinSuccess"
    readonly property string stateCheckinFailed: "checkinFailed"
    readonly property string stateOutcomeUnknown: "outcomeUnknown"
    readonly property string stateImportPreview: "importPreview"
    readonly property string stateCreateCheckoutFailed: "createCheckoutFailed"

    property string stateId: stateNormal

// When bound to the real read model, mutating Work actions are non-executing
// until their transactional controller is available. The read model renders truthfully but
    // no action here performs a repository/workspace mutation.
    property bool readOnlyBinding: false

    // Active Working Version (Work only) and Explore Inspected Version are
    // deliberately SEPARATE state. Switching one never changes the other.
    property string activeVersionId: "v1.7"
    property string inspectedVersionId: ""

    property string checkinMessage: ""
    property bool messageInvalid: false

    // True when the batch contains an unresolved conflict; blocks the whole batch.
    property bool reviewHasBlockingConflict: false
    // The batch is ephemeral; retained on failure for correction.
    property int batchCount: 4
    property bool batchRetained: false

    // Import: Replace Local is OFF by default and can never enable a managed dest.
    property bool replaceLocalDefault: false

    // Outcome-unknown: automatic retry is disabled; verification is required.
    readonly property bool autoRetryEnabled: false
    // Startup must never auto-checkout or auto-pick a fallback Active version.
    readonly property bool autoCheckoutOnStartup: false

    // One shared repository revision for the whole successful batch (never per item).
    property string successRevision: "r1420"

    // Create Derived Version composite (Create -> Checkout -> Set Active).
    property string createdVersionId: ""
    property bool checkoutFailed: false

    // Pending Active switch that is waiting on an explicit batch decision.
    property string pendingActiveSwitch: ""
    property bool requiresBatchDecision: false

    // Safe status summary for the bottom status bar.
    property string statusKind: "ready"
    property string statusText: "Ready"
    property string lastAction: "Make Editable · 3 files"

    signal activityLogRequested()

    function trimmed(text) {
        return String(text || "").replace(/^\s+|\s+$/g, "")
    }

    function activeVersion() {
        return root.model ? root.model.versionById(root.activeVersionId) : null
    }

    // ---- Checkin flow -----------------------------------------------------

    function beginReview() {
        // Enter Review. If the batch has a blocking conflict, land on the
        // conflict-blocked review that cannot progress to Confirm.
        stateId = reviewHasBlockingConflict ? stateConflictBlocked : stateReview
        lastAction = "Review Checkin · " + batchCount + " items"
    }

    function resolveConflict() {
        // Model an inline safe resolution (destination change / remove-from-batch).
        reviewHasBlockingConflict = false
        stateId = stateReview
    }

    function continueToConfirm() {
        // A blocking conflict blocks the whole batch: cannot reach Confirm.
        if (reviewHasBlockingConflict)
            return false
        stateId = stateConfirm
        return true
    }

    function confirmCheckin() {
        // Message is mandatory. Empty / whitespace-only is rejected even if the
        // GUI failed to disable Confirm.
        if (trimmed(checkinMessage).length === 0) {
            messageInvalid = true
            return false
        }
        messageInvalid = false
        // Whole-batch success: one shared revision.
        stateId = stateCheckinSuccess
        batchCount = 0
        batchRetained = false
        statusKind = "ready"
        statusText = "Checkin succeeded"
        lastAction = "Checkin · one revision " + successRevision
        return true
    }

    function markCheckinFailed() {
        // Failure before commit: repository unchanged, batch retained.
        stateId = stateCheckinFailed
        batchRetained = true
        statusKind = "error"
        statusText = "Checkin failed — no repository commit"
        lastAction = "Checkin failed · batch retained"
    }

    function markOutcomeUnknown() {
        // Lost response after commit may have occurred: claim neither outcome,
        // do not auto-retry.
        stateId = stateOutcomeUnknown
        batchRetained = true
        statusKind = "warn"
        statusText = "Commit outcome unknown — verification required"
        lastAction = "Checkin · outcome unknown"
    }

    function verifyOutcome() {
        // Verification is the ONLY route out of Outcome Unknown; there is no
        // direct blind Retry.
        statusText = "Verifying repository history…"
    }

    // ---- Import flow ------------------------------------------------------

    function openImport() {
        stateId = stateImportPreview
    }

    // Managed destinations are a hard block and can never be turned into an
    // allowed replacement by a GUI checkbox.
    function canReplace(destinationState) {
        return destinationState === "localExisting"
    }

    function isImportBlocked(destinationState) {
        return destinationState === "managed"
    }

    // ---- Create Derived Version composite --------------------------------

    function markCreateCheckoutFailed() {
        // Create succeeded but Checkout failed: keep the created repository
        // version, keep the previous Active Working Version, expose Retry Checkout.
        createdVersionId = "v1.8"
        checkoutFailed = true
        stateId = stateCreateCheckoutFailed
        statusKind = "warn"
        statusText = "Version created · Checkout failed"
        lastAction = "Create Derived · Checkout failed"
        // NOTE: activeVersionId intentionally left unchanged.
    }

    // ---- Active Working Version switching --------------------------------

    // Dirty local state (changed/new/missing/removed/LOCAL) does not by itself
    // block switching. A non-empty Batch is bound to its target and requires an
    // explicit discard/cancel decision before switching.
    function requestActiveSwitch(versionId) {
        if (batchCount > 0) {
            pendingActiveSwitch = versionId
            requiresBatchDecision = true
            return false
        }
        activeVersionId = versionId
        lastAction = "Active Working Version · " + versionId
        return true
    }

    function discardBatchAndSwitch() {
        if (pendingActiveSwitch.length === 0)
            return false
        batchCount = 0
        batchRetained = false
        activeVersionId = pendingActiveSwitch
        pendingActiveSwitch = ""
        requiresBatchDecision = false
        return true
    }

    function cancelActiveSwitch() {
        pendingActiveSwitch = ""
        requiresBatchDecision = false
    }

    function backToWork() {
        stateId = stateNormal
    }

    // ---- Deterministic preview/test state selection -----------------------

    function loadState(id) {
        // Reset the mutable fields to a known baseline, then apply the scenario.
        checkinMessage = ""
        messageInvalid = false
        reviewHasBlockingConflict = false
        batchRetained = false
        pendingActiveSwitch = ""
        requiresBatchDecision = false
        createdVersionId = ""
        checkoutFailed = false
        activeVersionId = "v1.7"
        inspectedVersionId = ""
        batchCount = 4
        statusKind = "ready"
        statusText = "Ready"
        lastAction = "Make Editable · 3 files"

        if (id === stateReview) {
            stateId = stateReview
        } else if (id === stateConflictBlocked) {
            reviewHasBlockingConflict = true
            stateId = stateConflictBlocked
        } else if (id === stateConfirm) {
            checkinMessage = "Update CPU synthesis constraints and reports"
            stateId = stateConfirm
        } else if (id === stateCheckinSuccess) {
            checkinMessage = "Update CPU synthesis constraints and reports"
            batchCount = 0
            statusKind = "ready"
            statusText = "Checkin succeeded"
            lastAction = "Checkin · one revision " + successRevision
            stateId = stateCheckinSuccess
        } else if (id === stateCheckinFailed) {
            checkinMessage = "Update CPU synthesis constraints and reports"
            batchRetained = true
            statusKind = "error"
            statusText = "Checkin failed — no repository commit"
            lastAction = "Checkin failed · batch retained"
            stateId = stateCheckinFailed
        } else if (id === stateOutcomeUnknown) {
            checkinMessage = "Update CPU synthesis constraints and reports"
            batchRetained = true
            statusKind = "warn"
            statusText = "Commit outcome unknown — verification required"
            lastAction = "Checkin · outcome unknown"
            stateId = stateOutcomeUnknown
        } else if (id === stateImportPreview) {
            stateId = stateImportPreview
        } else if (id === stateCreateCheckoutFailed) {
            createdVersionId = "v1.8"
            checkoutFailed = true
            statusKind = "warn"
            statusText = "Version created · Checkout failed"
            lastAction = "Create Derived · Checkout failed"
            stateId = stateCreateCheckoutFailed
        } else {
            stateId = stateNormal
        }
    }
}
