// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef QUICHE_SPDY_CORE_HPACK_HPACK_HEADER_TABLE_H_
#define QUICHE_SPDY_CORE_HPACK_HPACK_HEADER_TABLE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>

#include "absl/base/attributes.h"
#include "absl/hash/hash.h"
#include "absl/strings/string_view.h"
#include "common/platform/api/quiche_export.h"
#include "spdy/core/hpack/hpack_entry.h"
#include "spdy/platform/api/spdy_containers.h"

// All section references below are to http://tools.ietf.org/html/rfc7541.

namespace spdy {

namespace test {
class HpackHeaderTablePeer;
}  // namespace test

// Return value of GetByName() and GetByNameAndValue() if matching entry is not
// found.  This value is never used in HPACK for indexing entries, see
// https://httpwg.org/specs/rfc7541.html#index.address.space.
constexpr size_t kHpackEntryNotFound = 0;

// A data structure for the static table (2.3.1) and the dynamic table (2.3.2).
class QUICHE_EXPORT_PRIVATE HpackHeaderTable {
 public:
  friend class test::HpackHeaderTablePeer;

  // HpackHeaderTable takes advantage of the deque property that references
  // remain valid, so long as insertions & deletions are at the head & tail.
  // This precludes the use of base::circular_deque.
  //
  // If this changes (we want to change to circular_deque or we start to drop
  // entries from the middle of the table), this should to be a std::list, in
  // which case |*_index_| can be trivially extended to map to list iterators.
  using EntryTable = std::deque<HpackEntry>;

  struct QUICHE_EXPORT_PRIVATE EntryHasher {
    size_t operator()(const HpackEntry* entry) const;
  };
  struct QUICHE_EXPORT_PRIVATE EntriesEq {
    bool operator()(const HpackEntry* lhs, const HpackEntry* rhs) const;
  };
  using UnorderedEntrySet = SpdyHashSet<HpackEntry*, EntryHasher, EntriesEq>;
  using NameToEntryMap = SpdyHashMap<absl::string_view,
                                     const HpackEntry*,
                                     absl::Hash<absl::string_view>>;

  HpackHeaderTable();
  HpackHeaderTable(const HpackHeaderTable&) = delete;
  HpackHeaderTable& operator=(const HpackHeaderTable&) = delete;

  ~HpackHeaderTable();

  // Last-acknowledged value of SETTINGS_HEADER_TABLE_SIZE.
  size_t settings_size_bound() const { return settings_size_bound_; }

  // Current and maximum estimated byte size of the table, as described in
  // 4.1. Notably, this is /not/ the number of entries in the table.
  size_t size() const { return size_; }
  size_t max_size() const { return max_size_; }

  // The HPACK indexing scheme used by GetByIndex(), GetByName(), and
  // GetByNameAndValue() is defined at
  // https://httpwg.org/specs/rfc7541.html#index.address.space.

  // Returns the entry matching the index, or NULL.
  const HpackEntry* GetByIndex(size_t index);

  // Returns the index of the lowest-index entry matching |name|,
  // or kHpackEntryNotFound if no matching entry is found.
  size_t GetByName(absl::string_view name);

  // Returns the index of the lowest-index entry matching |name| and |value|,
  // or kHpackEntryNotFound if no matching entry is found.
  size_t GetByNameAndValue(absl::string_view name, absl::string_view value);

  // Sets the maximum size of the header table, evicting entries if
  // necessary as described in 5.2.
  void SetMaxSize(size_t max_size);

  // Sets the SETTINGS_HEADER_TABLE_SIZE bound of the table. Will call
  // SetMaxSize() as needed to preserve max_size() <= settings_size_bound().
  void SetSettingsHeaderTableSize(size_t settings_size);

  // Determine the set of entries which would be evicted by the insertion
  // of |name| & |value| into the table, as per section 4.4. No eviction
  // actually occurs. The set is returned via range [begin_out, end_out).
  void EvictionSet(absl::string_view name,
                   absl::string_view value,
                   EntryTable::iterator* begin_out,
                   EntryTable::iterator* end_out);

  // Adds an entry for the representation, evicting entries as needed. |name|
  // and |value| must not be owned by an entry which could be evicted. The
  // added HpackEntry is returned, or NULL is returned if all entries were
  // evicted and the empty table is of insufficent size for the representation.
  const HpackEntry* TryAddEntry(absl::string_view name,
                                absl::string_view value);

  void DebugLogTableState() const ABSL_ATTRIBUTE_UNUSED;

  // Returns the estimate of dynamically allocated memory in bytes.
  size_t EstimateMemoryUsage() const;

 private:
  // Returns number of evictions required to enter |name| & |value|.
  size_t EvictionCountForEntry(absl::string_view name,
                               absl::string_view value) const;

  // Returns number of evictions required to reclaim |reclaim_size| table size.
  size_t EvictionCountToReclaim(size_t reclaim_size) const;

  // Evicts |count| oldest entries from the table.
  void Evict(size_t count);

  // |static_entries_|, |static_index_|, and |static_name_index_| are owned by
  // HpackStaticTable singleton.

  // Tracks HpackEntries by index.
  const EntryTable& static_entries_;
  EntryTable dynamic_entries_;

  // Tracks the unique HpackEntry for a given header name and value.
  const UnorderedEntrySet& static_index_;

  // Tracks the first static entry for each name in the static table.
  const NameToEntryMap& static_name_index_;

  // Tracks the most recently inserted HpackEntry for a given header name and
  // value.
  UnorderedEntrySet dynamic_index_;

  // Tracks the most recently inserted HpackEntry for a given header name.
  NameToEntryMap dynamic_name_index_;

  // Last acknowledged value for SETTINGS_HEADER_TABLE_SIZE.
  size_t settings_size_bound_;

  // Estimated current and maximum byte size of the table.
  // |max_size_| <= |settings_size_bound_|
  size_t size_;
  size_t max_size_;

  // Total number of table insertions which have occurred,
  // including initial static table insertions.
  size_t total_insertions_;
};

}  // namespace spdy

#endif  // QUICHE_SPDY_CORE_HPACK_HPACK_HEADER_TABLE_H_
