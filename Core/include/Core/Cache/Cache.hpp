#pragma once

#include "Core/Cache/CacheStats.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

// Generic in-memory cache covering the fields AGENT.md #7 requires of
// every cache in this project: Key, Version (doubles as a content-hash
// comparison — callers pass whichever fits), Timestamp, Expiration
// (TTL), Invalidation, and Dependency. Intended as the shared foundation
// File Cache / AST Cache / Symbol Cache / Embedding Cache (Phase 2
// Context Engine) build on, rather than each hand-rolling its own.
//
// Thread-safe (single internal mutex) but intentionally simple: no LRU
// eviction or size limits yet — add those when a concrete cache actually
// needs them (AGENT.md #14 — minimal implementation first).
template <typename T>
class Cache {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using NowFn = std::function<TimePoint()>;

    struct PutOptions {
        // A version or content-hash string. If set, a later Get(key, version)
        // with a different value is treated as a miss and evicts the entry.
        std::string version;
        // Time-to-live from the moment of Put(); nullopt means "never expires".
        std::optional<std::chrono::milliseconds> ttl;
        // Other cache keys/tags this entry is derived from. See
        // InvalidateByDependency() — e.g. an AST cache entry might depend
        // on the source file's cache key, so a File Cache invalidation
        // cascades to it.
        std::vector<std::string> dependencies;
    };

    explicit Cache(NowFn now_fn = [] { return Clock::now(); }) : now_fn_(std::move(now_fn)) {}

    void Put(const std::string& key, T value, PutOptions options = {}) {
        std::lock_guard lock(mutex_);
        Entry entry;
        entry.value = std::move(value);
        entry.version = std::move(options.version);
        entry.created_at = now_fn_();
        entry.expires_at = options.ttl ? std::optional(entry.created_at + *options.ttl) : std::nullopt;
        entry.dependencies = std::move(options.dependencies);
        entries_.insert_or_assign(key, std::move(entry));
    }

    // Returns the cached value, or nullopt on miss (absent or expired).
    [[nodiscard]] std::optional<T> Get(const std::string& key) {
        std::lock_guard lock(mutex_);
        return GetLocked(key, nullptr);
    }

    // Returns the cached value only if its stored version/hash matches
    // `expected_version`; otherwise treats it as a miss and evicts the
    // stale entry.
    [[nodiscard]] std::optional<T> Get(const std::string& key, const std::string& expected_version) {
        std::lock_guard lock(mutex_);
        return GetLocked(key, &expected_version);
    }

    [[nodiscard]] bool Contains(const std::string& key) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(key);
        if (it == entries_.end()) {
            return false;
        }
        if (IsExpired(it->second)) {
            entries_.erase(it);
            ++stats_.evictions;
            return false;
        }
        return true;
    }

    void Invalidate(const std::string& key) {
        std::lock_guard lock(mutex_);
        if (entries_.erase(key) > 0) {
            ++stats_.invalidations;
        }
    }

    // Removes every entry whose PutOptions::dependencies contained `dependency`.
    void InvalidateByDependency(const std::string& dependency) {
        std::lock_guard lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            const auto& deps = it->second.dependencies;
            if (std::find(deps.begin(), deps.end(), dependency) != deps.end()) {
                it = entries_.erase(it);
                ++stats_.invalidations;
            } else {
                ++it;
            }
        }
    }

    void Clear() {
        std::lock_guard lock(mutex_);
        stats_.invalidations += entries_.size();
        entries_.clear();
    }

    [[nodiscard]] CacheStats Stats() const {
        std::lock_guard lock(mutex_);
        CacheStats stats = stats_;
        stats.size = entries_.size();
        return stats;
    }

private:
    struct Entry {
        T value;
        std::string version;
        TimePoint created_at;
        std::optional<TimePoint> expires_at;
        std::vector<std::string> dependencies;
    };

    [[nodiscard]] bool IsExpired(const Entry& entry) const {
        return entry.expires_at.has_value() && now_fn_() >= *entry.expires_at;
    }

    std::optional<T> GetLocked(const std::string& key, const std::string* expected_version) {
        const auto it = entries_.find(key);
        if (it == entries_.end()) {
            ++stats_.misses;
            return std::nullopt;
        }
        if (IsExpired(it->second)) {
            entries_.erase(it);
            ++stats_.evictions;
            ++stats_.misses;
            return std::nullopt;
        }
        if (expected_version != nullptr && it->second.version != *expected_version) {
            entries_.erase(it);
            ++stats_.evictions;
            ++stats_.misses;
            return std::nullopt;
        }
        ++stats_.hits;
        return it->second.value;
    }

    NowFn now_fn_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    CacheStats stats_;
};

} // namespace aistudio::core
