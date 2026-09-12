#pragma once

#include <cstdint>
#include <string>

namespace aistudio::core {

// Everything File Cache / Incremental Reload need to decide whether a
// file changed, without re-reading its content (docs/ROADMAP.md Phase 2
// "File Metadata"). `content_hash` is FNV-1a (see HashContent in
// FileScanner.hpp) — a fast change-detection hash, not cryptographic.
struct FileMetadata {
    std::string path; // relative to the scanned root, '/'-separated
    std::uint64_t size = 0;
    std::int64_t last_write_time = 0; // filesystem-clock ticks; comparable only within one machine/run
    std::string content_hash;
};

} // namespace aistudio::core
