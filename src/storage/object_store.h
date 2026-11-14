#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace isvn::storage {

struct PutResult {
    std::string hash;
    bool deduped;
};

class ObjectStore {
public:
    explicit ObjectStore(const std::filesystem::path& pool_root);
    ~ObjectStore();

    ObjectStore(const ObjectStore&) = delete;
    ObjectStore& operator=(const ObjectStore&) = delete;
    ObjectStore(ObjectStore&&) noexcept;
    ObjectStore& operator=(ObjectStore&&) noexcept;

    PutResult put_object(const std::filesystem::path& source_file);
    void get_object(const std::string& hash, const std::filesystem::path& dest) const;
    std::filesystem::path object_path(const std::string& hash) const;
    bool has_object(const std::string& hash) const;

    void register_ref(const std::string& hash, const std::string& link_path);
    void unregister_ref(const std::string& link_path);
    // True when a ref row exists for this absolute link path.
    bool has_ref(const std::string& link_path) const;
    // The object hash recorded for this absolute link path, if any.
    std::optional<std::string> ref_hash(const std::string& link_path) const;
    int refcount(const std::string& hash) const;

    bool verify_object(const std::string& hash) const;
    void touch_last_access(const std::string& hash);
    std::string last_access_of(const std::string& hash) const;
    void record_renew_event(const std::string& hash) noexcept;
    bool atime_usable() const;
    bool is_dangling_link(const std::string& link_path) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace isvn::storage
