import QtQuick

// WorkMockModel — typed mock state for the Work UI.
//
// This is mock/test data only. It does NOT bind to any backend command, CLI
// bridge, role resolver, SVN, or filesystem. The field shapes match the data
// that WorkReadController provides from the CLI, so the Work components render
// either source unchanged.
//
// Semantics that are NOT represented here: per-item checkin revisions, a
// placeholder shared revision, a partly-committed Checkin batch, automatic
// startup checkout, automatic fallback Active version, managed-destination
// replacement, and import creating a repository revision.
QtObject {
    id: root

    // Fixed, non-editable project/workspace identity. There is no project
    // selector: one GUI process is bound to one project and one workspace.
    readonly property var identity: ({
        "projectName": "chip_alpha",
        "workspacePath": "workspace/chip_alpha",
        "role": "PM",
        "buildIdentity": "isvn 2.4.0-mock",
        "backendState": "ready"
    })

    // Locally checked-out working versions only (never repository-wide branches).
    // Real hierarchy Phase -> Block -> Stage -> Version. Exactly one isActive.
    readonly property var localVersions: [
        {
            "id": "v1.7", "versionName": "v1.7",
            "phase": "Phase_2", "block": "block_cpu", "stage": "SYN",
            "repositoryPath": "Phase_2/block_cpu/synthesis/v1.7",
            "localPath": "workspace/chip_alpha/Phase_2/block_cpu/synthesis/v1.7",
            "changedCount": 3, "conflictCount": 1, "localCount": 2,
            "lastUsedAt": "just now", "isActive": true
        },
        {
            "id": "v1.6", "versionName": "v1.6",
            "phase": "Phase_2", "block": "block_cpu", "stage": "SYN",
            "repositoryPath": "Phase_2/block_cpu/synthesis/v1.6",
            "localPath": "workspace/chip_alpha/Phase_2/block_cpu/synthesis/v1.6",
            "changedCount": 0, "conflictCount": 0, "localCount": 0,
            "lastUsedAt": "2h ago", "isActive": false
        },
        {
            "id": "v0.9", "versionName": "v0.9",
            "phase": "Phase_2", "block": "block_mem", "stage": "RTL",
            "repositoryPath": "Phase_2/block_mem/rtl/v0.9",
            "localPath": "workspace/chip_alpha/Phase_2/block_mem/rtl/v0.9",
            "changedCount": 2, "conflictCount": 0, "localCount": 1,
            "lastUsedAt": "1d ago", "isActive": false
        },
        {
            "id": "v1.2", "versionName": "v1.2",
            "phase": "Phase_1", "block": "block_io", "stage": "ROUTE",
            "repositoryPath": "Phase_1/block_io/route/v1.2",
            "localPath": "workspace/chip_alpha/Phase_1/block_io/route/v1.2",
            "changedCount": 0, "conflictCount": 0, "localCount": 0,
            "lastUsedAt": "6d ago", "isActive": false
        }
    ]

    // File status of the Active Working Version. state is the client-side UI
    // enum; sourceKind is LINKED/LOCAL and is never inferred from color.
    // availability is a backend-owned shape (allowed + reasonCode), mocked here.
    readonly property var activeFiles: [
        { "path": "rtl", "kind": "folder", "state": "", "sourceKind": "", "sizeBytes": -1,
          "selected": false, "availability": { "makeEditable": false, "reasonCode": "FOLDER" } },
        { "path": "rtl/cpu_core.sv", "kind": "file", "state": "modified", "sourceKind": "LINKED", "sizeBytes": 12698,
          "selected": true, "availability": { "makeEditable": true, "addToBatch": true, "revert": true, "remove": true, "reasonCode": "" } },
        { "path": "rtl/alu.sv", "kind": "file", "state": "editable", "sourceKind": "LOCAL", "sizeBytes": 8909,
          "selected": true, "availability": { "makeEditable": false, "addToBatch": true, "revert": true, "remove": true, "reasonCode": "ALREADY_EDITABLE" } },
        { "path": "constraints", "kind": "folder", "state": "", "sourceKind": "", "sizeBytes": -1,
          "selected": false, "availability": { "makeEditable": false, "reasonCode": "FOLDER" } },
        { "path": "constraints/cpu_top.sdc", "kind": "file", "state": "modified", "sourceKind": "LINKED", "sizeBytes": 3174,
          "selected": true, "availability": { "makeEditable": true, "addToBatch": true, "revert": true, "remove": true, "reasonCode": "" } },
        { "path": "reports", "kind": "folder", "state": "", "sourceKind": "", "sizeBytes": -1,
          "selected": false, "availability": { "makeEditable": false, "reasonCode": "FOLDER" } },
        { "path": "reports/timing.rpt", "kind": "file", "state": "new", "sourceKind": "LOCAL", "sizeBytes": 46694,
          "selected": true, "availability": { "makeEditable": false, "addToBatch": true, "revert": false, "remove": true, "reasonCode": "LOCAL_NEW" } },
        { "path": "scripts/run_synth.tcl", "kind": "file", "state": "conflict", "sourceKind": "LOCAL", "sizeBytes": 2355,
          "selected": false, "availability": { "makeEditable": false, "addToBatch": false, "revert": false, "remove": true, "reasonCode": "CONFLICT_MANAGED_COLLISION" } },
        { "path": "docs/review_notes.md", "kind": "file", "state": "new", "sourceKind": "LOCAL", "sizeBytes": 1229,
          "selected": false, "availability": { "makeEditable": false, "addToBatch": true, "revert": false, "remove": true, "reasonCode": "" } }
    ]

    // Ephemeral Checkin Batch entries bound to the Active Working Version.
    // readiness: ready | conflict | revalidate | failed. issueCode is a stable
    // reason code; the conflict item models a LOCAL->managed collision that must
    // be resolved (never silently overwritten).
    readonly property var batchEntries: [
        { "sourcePath": "rtl/cpu_core.sv", "destinationPath": "rtl/cpu_core.sv",
          "changeKind": "modified", "sourceKind": "LINKED", "sizeBytes": 12698,
          "readiness": "ready", "issueCode": "", "issueMessage": "" },
        { "sourcePath": "constraints/cpu_top.sdc", "destinationPath": "constraints/cpu_top.sdc",
          "changeKind": "modified", "sourceKind": "LINKED", "sizeBytes": 3174,
          "readiness": "ready", "issueCode": "", "issueMessage": "" },
        { "sourcePath": "reports/timing.rpt", "destinationPath": "reports/timing.rpt",
          "changeKind": "new", "sourceKind": "LOCAL", "sizeBytes": 46694,
          "readiness": "ready", "issueCode": "", "issueMessage": "" },
        { "sourcePath": "scripts/run_synth.tcl", "destinationPath": "scripts/run_synth.tcl",
          "changeKind": "conflict", "sourceKind": "LOCAL", "sizeBytes": 2355,
          "readiness": "conflict", "issueCode": "MANAGED_COLLISION",
          "issueMessage": "A LOCAL file cannot overwrite an existing managed file." }
    ]

    // External import preview items. destinationState: absent | managed |
    // localExisting. The managed entry is a hard block and can never be turned
    // into an allowed replacement by a GUI checkbox.
    readonly property var importItems: [
        { "sourcePath": "signoff_bundle/netlist/cpu_top.v.gz",
          "destinationPath": "netlist/cpu_top.v.gz", "sizeBytes": 33135165,
          "destinationState": "absent", "replaceLocal": false },
        { "sourcePath": "signoff_bundle/reports/qor_summary.rpt",
          "destinationPath": "reports/qor_summary.rpt", "sizeBytes": 149504,
          "destinationState": "absent", "replaceLocal": false },
        { "sourcePath": "signoff_bundle/constraints/io_delay.sdc",
          "destinationPath": "constraints/io_delay.sdc", "sizeBytes": 8397,
          "destinationState": "localExisting", "replaceLocal": false },
        { "sourcePath": "signoff_bundle/rtl/cpu_core.sv",
          "destinationPath": "rtl/cpu_core.sv", "sizeBytes": 12698,
          "destinationState": "managed", "replaceLocal": false }
    ]

    // Import destination root is always the Active Working Version root.
    readonly property string importSourceRoot: "signoff_bundle/"

    function humanSize(bytes) {
        if (bytes === undefined || bytes === null || bytes < 0)
            return ""
        if (bytes < 1024)
            return bytes + " B"
        if (bytes < 1024 * 1024)
            return (bytes / 1024).toFixed(1) + " KB"
        if (bytes < 1024 * 1024 * 1024)
            return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(1) + " GB"
    }

    function versionById(id) {
        for (var i = 0; i < localVersions.length; ++i) {
            if (localVersions[i].id === id)
                return localVersions[i]
        }
        return null
    }
}
