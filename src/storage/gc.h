#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace isvn::storage {

struct PhantomRef {
    std::string hash;
    std::string link_path;
};

// A reference kept because this process could not prove that its link is gone.
// reason is what this filesystem view showed at the link path:
// "permission_denied", "not_found", "replaced", "io_error", or
// "unverified_path" when the path to the link inside the workspace passes
// through a symlink or another filesystem.
// scope says why that was not enough to release it: "no_scope", "other_owner",
// "outside_workspace", or "in_scope" when the link itself could not be checked.
struct RetainedRef {
    std::string hash;
    std::string link_path;
    std::string owner;
    std::string reason;
    std::string scope;
};

struct ReclaimFailure {
    std::string hash;
    std::string detail;
};

struct GcReport {
    int refs_removed = 0;
    int objects_reclaimed = 0;
    int objects_force_evicted = 0;
    std::int64_t bytes_freed = 0;
    bool low_watermark_reached = true;
    std::vector<PhantomRef> phantom_refs;
    // Refs that were neither verified live nor provably gone; they are left in
    // place and their refcount still counts them.
    std::vector<RetainedRef> retained_refs;
    // Objects whose refcount reached zero but could not be removed; their rows
    // are kept and the cleanup is incomplete.
    std::vector<ReclaimFailure> reclaim_failures;
};

// Defines which references a reconcile may release.  A pool is shared by
// accounts whose workspaces may be unreadable or absent in this filesystem view
// (another account's private directory, another server's local disk), so a link
// that cannot be inspected is never treated as removed.  A ref is released only
// when it was recorded for `owner`, its link lies inside `workspace`, and the
// link is visibly absent or replaced there, as seen by walking down from the
// workspace root without following symlinks or crossing into another
// filesystem.  `workspace` must be an iSVN workspace root (containing
// .isvn_local) owned by the calling user; otherwise the scope grants nothing.
struct ReconcileScope {
    std::string owner;
    std::filesystem::path workspace;
};

class GarbageCollector {
public:
    // Without a scope, reconcile only verifies refs; it never releases one.
    explicit GarbageCollector(const std::filesystem::path& pool_root);
    GarbageCollector(const std::filesystem::path& pool_root, ReconcileScope scope);
    ~GarbageCollector();

    GarbageCollector(const GarbageCollector&) = delete;
    GarbageCollector& operator=(const GarbageCollector&) = delete;
    GarbageCollector(GarbageCollector&&) noexcept;
    GarbageCollector& operator=(GarbageCollector&&) noexcept;

    GcReport reconcile();
    GcReport reconcileDryRun();
    GcReport reclaim(double high_watermark, double low_watermark);
    GcReport run(double high_watermark, double low_watermark);
    // Reconcile then delete only refcount==0 objects; no watermark gate, no forced
    // eviction. Used to immediately free space orphaned by a logical delete.  An
    // object that cannot be removed is recorded in reclaim_failures and does not
    // stop the others.
    GcReport reclaimOrphans();
    // Reconcile, then delete only the listed objects (and objects whose refs this
    // reconcile released) when their refcount is zero.  Objects that were already
    // unreferenced before the caller released anything are left alone.  A failure
    // to remove one object is recorded in reclaim_failures and does not stop the
    // others.
    GcReport reclaimReleased(const std::vector<std::string>& released_hashes);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace isvn::storage
