#include "Core/Context/ContextCache.hpp"

#include <cstdint>
#include <sstream>

namespace aistudio::core {

namespace {

std::string BuildKey(const ContextRetriever& retriever, const std::string& intent) {
    std::ostringstream key;
    key << reinterpret_cast<std::uintptr_t>(&retriever) << '\x1f' << intent << '\x1f'
        << retriever.CurrentActiveDocumentPath().value_or("");
    return key.str();
}

} // namespace

std::vector<ContextItem> ContextCache::GetOrRetrieve(const ContextRetriever& retriever, const std::string& intent) {
    // Known limitation (code review, 2026-09-05): Get()+Put() here are each
    // individually locked by Cache<T>, but the check-then-compute-then-put
    // sequence as a whole is not atomic. Two concurrent callers for the
    // same intent can both observe a miss and both call Retrieve()
    // (correctness is preserved, but it's a thundering-herd duplication of
    // work, and Stats() undercounts hits). Not currently reachable: the
    // sole call site (main.cpp bootstrap) is single-threaded, and
    // McpServer::Run() is explicitly documented as blocking/single-threaded
    // with no concurrent pipelining. If ContextCache is ever called from
    // multiple threads, this sequence needs a per-key lock (or an
    // in-flight-computation map) to avoid the duplicated work.
    const std::string key = BuildKey(retriever, intent);
    const std::string version = std::to_string(generation_);
    if (auto cached = cache_.Get(key, version); cached.has_value()) {
        return *cached;
    }

    std::vector<ContextItem> items = retriever.Retrieve(intent);
    cache_.Put(key, items, {.version = version});
    return items;
}

void ContextCache::InvalidateAll() {
    ++generation_;
}

void ContextCache::Invalidate(const ContextRetriever& retriever, const std::string& intent) {
    cache_.Invalidate(BuildKey(retriever, intent));
}

CacheStats ContextCache::Stats() const {
    return cache_.Stats();
}

} // namespace aistudio::core
