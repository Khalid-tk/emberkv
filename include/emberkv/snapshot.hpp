#pragma once

// Point-in-time persistence (SAVE / BGSAVE / load on startup).
//
// File format, all integers little-endian so a snapshot written on one
// machine loads on any other:
//
//   "EMBERKV\0"            8-byte magic
//   u32 version            currently 1
//   record*                one per key
//   u8 0xFF                end marker
//   u64 checksum           FNV-1a over every preceding byte
//
//   record   = u8 type (0 string, 1 list) | i64 expiry (unix ms, -1 = none)
//              | str key | payload
//   payload  = str                          (string)
//            | u32 count, str * count       (list)
//   str      = u32 length, bytes
//
// TTLs are stored as wall-clock deadlines: the in-memory store uses a
// monotonic clock, which means nothing across a restart.

#include "emberkv/store.hpp"

#include <cstddef>
#include <filesystem>
#include <stdexcept>

namespace emberkv {

class SnapshotError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct SnapshotInfo {
    std::size_t keys = 0;
    std::size_t bytes = 0;
};

/// Writes to `<path>.tmp`, flushes, then renames over `path`, so a crash
/// mid-save never leaves a truncated snapshot behind.
SnapshotInfo save_snapshot(Store& store, const std::filesystem::path& path);

/// Loads every non-expired key into `store`. Verifies the checksum before
/// touching the store, so a corrupt file changes nothing.
SnapshotInfo load_snapshot(Store& store, const std::filesystem::path& path);

}  // namespace emberkv
