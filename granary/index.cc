/* Copyright 2014 Peter Goodman, all rights reserved. */

#define GRANARY_INTERNAL

#include "granary/base/new.h"
#include "granary/base/string.h"

#include "granary/app.h"
#include "granary/breakpoint.h"
#include "granary/cache.h"
#include "granary/index.h"
#include "granary/metadata.h"

#include "os/memory.h"

namespace granary {
namespace {

// Tombstone pointer representing the last meta-data block in a meta-data array.
static BlockMetaData * const kMetaArrayEnd = \
    reinterpret_cast<BlockMetaData *>(1ULL);

}  // namespace
namespace internal {

enum {
  DEALLOCATED_MEMORY_POISON = 0xFA
};

void *IndexArrayMem::operator new(std::size_t) {
  return memset(os::AllocatePages(1), 0, arch::PAGE_SIZE_BYTES);
}

void IndexArrayMem::operator delete(void *address) {
  memset(address, DEALLOCATED_MEMORY_POISON, arch::PAGE_SIZE_BYTES);
  return os::FreePages(address, 1);
}

class MetaDataArray : public IndexArrayMem {
 public:
  // Deletes all meta-data linked into this array.
  ~MetaDataArray(void) {
    for (auto meta : metas) {
      while (meta && kMetaArrayEnd != meta) {
        auto index_meta = MetaDataCast<IndexMetaData *>(meta);
        auto next_meta = index_meta->next;
        delete meta;
        meta = next_meta;
      }
    }
  }

  BlockMetaData *metas[NUM_POINTERS_PER_PAGE];
};

static_assert(sizeof(MetaDataArray) == arch::PAGE_SIZE_BYTES,
              "The size of `MetaDataArray` must be exactly one page.");

}  // namespace internal
namespace {

enum {
  NUM_IGNORED_BITS = 3,
  MAX_FIRST_INDEX = internal::NUM_POINTERS_PER_PAGE,
  NUM_BITS_PER_ARRAY = __builtin_popcount(MAX_FIRST_INDEX - 1)
};

// Represents the index levels for some meta-data.
struct MetaDataIndex {
  uintptr_t first;
  uintptr_t second;

  inline bool operator!=(const MetaDataIndex &that) const {
    return first != that.first || second != that.second;
  }
};

static MetaDataIndex AddrToIndex(uintptr_t addr) {
  return {
    (addr >> (NUM_IGNORED_BITS + NUM_BITS_PER_ARRAY)) % MAX_FIRST_INDEX,
    (addr >> NUM_IGNORED_BITS) % internal::NUM_POINTERS_PER_PAGE
  };
}

static MetaDataIndex NextIndex(MetaDataIndex index) {
  if (internal::NUM_POINTERS_PER_PAGE == (index.second + 1)) {
    return {
      (index.first + 1) % MAX_FIRST_INDEX,
      0
    };
  } else {
    return {
      index.first,
      index.second + 1
    };
  }
}

// Returns the index into the code cache for a given piece of meta-data.
static MetaDataIndex GetIndex(BlockMetaData *meta) {
  const auto app_meta = MetaDataCast<AppMetaData *>(meta);
  return AddrToIndex(reinterpret_cast<uintptr_t>(app_meta->start_pc));
}

static AppPC GetAppPC(BlockMetaData *meta) {
  return meta ? MetaDataCast<AppMetaData *>(meta)->start_pc : nullptr;
}

typedef MetaDataLinkedListIterator<IndexMetaData> IndexMetaDataIterator;

// Match some meta-data that we are search for (`search`) against a linked
// list of potential meta-data.
static IndexFindResponse MatchMetaData(BlockMetaData *ls,
                                       BlockMetaData *search) {
  IndexFindResponse response = {
    UnificationStatus::REJECT,
    nullptr
  };
  for (auto meta : IndexMetaDataIterator(ls)) {
    if (kMetaArrayEnd == meta) break;
    if (search->Equals(meta)) {
      switch (search->CanUnifyWith(ls)) {
        case UnificationStatus::ACCEPT:
          response.status = UnificationStatus::ACCEPT;
          response.meta = meta;
          return response;

        case UnificationStatus::ADAPT:
          if (UnificationStatus::ADAPT != response.status) {
            response.status = UnificationStatus::ADAPT;
            response.meta = meta;
          }
          break;

        case UnificationStatus::REJECT:
          break;
      }
    }
  }
  return response;
}

// Unlinks meta-data that falls in the range `[begin, end)`.
//
// TODO(pag): This doesn't handle the case where a block begins before `begin`
//            but ends inside of `[begin, end)`. For now I will assume this
//            doesn't happen in practice. One exception might be JITs.
static BlockMetaData *UnlinkMetaData(BlockMetaData **prev_ptr,
                                     BlockMetaData *removed,
                                     AppPC begin, AppPC end) {
  auto meta = *prev_ptr;
  while (meta && kMetaArrayEnd != meta) {
    auto index_meta = MetaDataCast<IndexMetaData *>(meta);
    auto next_meta = index_meta->next;
    auto meta_pc = GetAppPC(meta);
    if (begin <= meta_pc && meta_pc < end) {
      *prev_ptr = next_meta;  // Unlink.
      index_meta->next = removed;
      removed = index_meta->next;
    } else {
      prev_ptr = &(index_meta->next);
    }
    meta = next_meta;
  }
  return removed;
}

}  // namespace

Index::~Index(void) {
  for (auto array : arrays) {
    delete array;
  }
}

// Perform a lookup operation in the code cache index. Lookup operations might
// not return exact matches, as hinted at by the `status` field of the
// `IndexFindResponse` structure. This has to do with block unification.
IndexFindResponse Index::Request(BlockMetaData *meta) {
  if (GRANARY_UNLIKELY(!meta)) {
    return {UnificationStatus::REJECT, meta};
  }

  auto index_meta = MetaDataCast<IndexMetaData *>(meta);
  if (index_meta->next) {
    return {UnificationStatus::ACCEPT, meta};  // It must be in the index.
  }

  const auto index = GetIndex(meta);
  if (auto array = arrays[index.first]) {
    if (auto metas = array->metas[index.second]) {
      return MatchMetaData(metas, meta);
    }
  }
  return {UnificationStatus::REJECT, nullptr};  // Not in the index
}

// Insert a block into the code cache index.
void Index::Insert(BlockMetaData *meta) {
  GRANARY_ASSERT(nullptr != MetaDataCast<AppMetaData *>(meta)->start_pc);
  GRANARY_ASSERT(nullptr != MetaDataCast<CacheMetaData *>(meta)->start_pc);

  auto index_meta = MetaDataCast<IndexMetaData *>(meta);
  if (index_meta->next) return;

  const auto index = GetIndex(meta);
  auto &array(arrays[index.first]);
  if (GRANARY_UNLIKELY(!array)) array = new internal::MetaDataArray;

  auto &metas(array->metas[index.second]);
  if (GRANARY_UNLIKELY(!metas)) metas = kMetaArrayEnd;

  index_meta->next = metas;  // Add it in.
  metas = meta;
}

// Remove all meta-data (from the index) associated with any application
// code falling in the address range `[begin, end)`. Returns a pointer to
// a linked list (via `IndexMetaData`) of all removed block meta-data.
BlockMetaData *Index::RemoveRange(AppPC begin, AppPC end) {
  GRANARY_ASSERT(begin <= end);
  BlockMetaData *ret(nullptr);
  auto index = AddrToIndex(reinterpret_cast<uintptr_t>(begin));
  auto end_index = AddrToIndex(reinterpret_cast<uintptr_t>(end));

  for (; index != end_index; index = NextIndex(index)) {
    auto array = arrays[index.first];
    if (!array) continue;
    auto &metas(array->metas[index.second]);
    if (!metas) continue;
    ret = UnlinkMetaData(&metas, ret, begin, end);
  }
  return ret;
}

}  // namespace granary
