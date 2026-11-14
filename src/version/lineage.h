#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace isvn::version {

struct LineageEntry {
    std::string child;
    std::optional<std::string> parent;
    std::string actor;
    std::string created_at;
    std::string origin;
    std::optional<std::string> note;
};

// One immutable row from lineage_audit. The fields intentionally mirror the
// table and do not join SVN/cache/hash-derived data.
struct AuditRecord {
    std::int64_t seq;
    std::string ts;
    std::string actor;
    std::string action;
    std::string child;
    std::optional<std::string> old_parent;
    std::optional<std::string> new_parent;
    std::optional<std::string> reason;
};

using LineageAuditEntry = AuditRecord;

struct IntegrityResult {
    bool ok;
    std::optional<std::int64_t> first_bad_seq;
    std::string detail;
};

class Lineage {
public:
    explicit Lineage(const std::filesystem::path& project_root);
    ~Lineage();

    Lineage(const Lineage&) = delete;
    Lineage& operator=(const Lineage&) = delete;
    Lineage(Lineage&&) noexcept;
    Lineage& operator=(Lineage&&) noexcept;

    void recordParent(const std::string& child,
                      const std::optional<std::string>& parent,
                      const std::string& reason,
                      const std::optional<std::string>& note = std::nullopt);

    void resolveParent(const std::string& child,
                       const std::optional<std::string>& new_parent,
                       const std::string& reason,
                       const std::optional<std::string>& note);

    std::optional<std::string> parentOf(const std::string& child) const;
    std::vector<std::string> ancestry(const std::string& child) const;
    std::string ancestryJson(const std::string& child) const;

    bool hasDescendants(const std::string& target) const;
    std::vector<std::string> directChildrenOf(const std::string& target) const;

    void removeEntry(const std::string& child, const std::string& reason);

    std::vector<AuditRecord> auditTrail(
        const std::optional<std::string>& child = std::nullopt) const;

    IntegrityResult verifyIntegrity() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace isvn::version
