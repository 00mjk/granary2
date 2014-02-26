/* Copyright 2014 Peter Goodman, all rights reserved. */

#define GRANARY_INTERNAL

#include "granary/base/list.h"
#include "granary/base/lock.h"
#include "granary/base/string.h"

#include "granary/breakpoint.h"
#include "granary/metadata.h"

namespace granary {

// Initialize Granary's internal translation cache meta-data.
CacheMetaData::CacheMetaData(void)
    : cache_pc(nullptr) {}

namespace {
// The next meta-data description ID that we can assign. Every meta-data
// description has a unique, global ID.
static int next_description_id(0);

// A lock on assigning and ID to any description, as well as to updating the
// `next_description_id` variable.
static FineGrainedLock next_description_id_lock;
}  // namespace

// Cast some generic meta-data into some specific meta-data.
void *BlockMetaData::Cast(MetaDataDescription *desc) {
  GRANARY_ASSERT(-1 != desc->id);
  GRANARY_ASSERT(nullptr != manager->descriptions[desc->id]);
  auto meta_ptr = reinterpret_cast<uintptr_t>(this);
  return reinterpret_cast<void *>(meta_ptr + manager->offsets[desc->id]);
}

// Initialize a new meta-data instance. This involves separately initializing
// the contained meta-data within this generic meta-data.
BlockMetaData::BlockMetaData(MetaDataManager *manager_)
    : manager(manager_) {
  auto this_ptr = reinterpret_cast<uintptr_t>(this);
  for (auto desc : manager->descriptions) {
    if (desc) {
      auto offset = manager->offsets[desc->id];
      desc->initialize(reinterpret_cast<void *>(this_ptr + offset));
    }
  }
}

// Destroy a meta-data instance. This involves separately destroying the
// contained meta-data within this generic meta-data.
BlockMetaData::~BlockMetaData(void) {
  auto this_ptr = reinterpret_cast<uintptr_t>(this);
  for (auto desc : manager->descriptions) {
    if (desc) {
      auto offset = manager->offsets[desc->id];
      desc->destroy(reinterpret_cast<void *>(this_ptr + offset));
    }
  }
}

// Create a copy of some meta-data and return a new instance of the copied
// meta-data.
BlockMetaData *BlockMetaData::Copy(void) const {
  auto this_ptr = reinterpret_cast<uintptr_t>(this);
  auto that_ptr = reinterpret_cast<uintptr_t>(manager->Allocate());
  for (auto desc : manager->descriptions) {
    if (desc) {
      auto offset = manager->offsets[desc->id];
      desc->copy_initialize(reinterpret_cast<void *>(that_ptr + offset),
                            reinterpret_cast<const void *>(this_ptr + offset));
    }
  }

  return reinterpret_cast<BlockMetaData *>(that_ptr);
}

// Hash all serializable meta-data contained within this generic meta-data.
void BlockMetaData::Hash(HashFunction *hasher) const {
  auto this_ptr = reinterpret_cast<uintptr_t>(this);
  for (auto desc : manager->descriptions) {
    if (desc && desc->hash) {
      auto offset = manager->offsets[desc->id];
      desc->hash(hasher, reinterpret_cast<const void *>(this_ptr + offset));
    }
  }
}

// Compare the serializable components of two generic meta-data instances for
  // strict equality.
bool BlockMetaData::Equals(const BlockMetaData *that) const {
  auto this_ptr = reinterpret_cast<uintptr_t>(this);
  auto that_ptr = reinterpret_cast<uintptr_t>(that);
  for (auto desc : manager->descriptions) {
    if (desc && desc->compare_equals) {  // Indexable.
      auto offset = manager->offsets[desc->id];
      auto this_meta = reinterpret_cast<const void *>(this_ptr + offset);
      auto that_meta = reinterpret_cast<const void *>(that_ptr + offset);
      if (!desc->compare_equals(this_meta, that_meta)) {
        return false;
      }
    }
  }
  return true;
}

// Check to see if this meta-data can unify with some other generic meta-data.
UnificationStatus BlockMetaData::CanUnifyWith(
    const BlockMetaData *that) const {
  auto this_ptr = reinterpret_cast<uintptr_t>(this);
  auto that_ptr = reinterpret_cast<uintptr_t>(that);
  auto can_unify = UnificationStatus::ACCEPT;
  for (auto desc : manager->descriptions) {
    if (desc && desc->can_unify) {  // Unifiable.
      auto offset = manager->offsets[desc->id];
      auto this_meta = reinterpret_cast<const void *>(this_ptr + offset);
      auto that_meta = reinterpret_cast<const void *>(that_ptr + offset);
      auto local_can_unify = desc->can_unify(this_meta, that_meta);
      can_unify = GRANARY_MAX(can_unify, local_can_unify);
    }
  }
  return can_unify;
}

// Dynamically free meta-data.
void BlockMetaData::operator delete(void *address) {
  auto self = reinterpret_cast<BlockMetaData *>(address);
  self->manager->Free(self);
}

// Initialize an empty metadata manager.
MetaDataManager::MetaDataManager(void)
    : size(sizeof(BlockMetaData)),
      is_finalized(false),
      allocator() {
  memset(&(descriptions[0]), 0, sizeof(descriptions));
  memset(&(offsets[0]), 0, sizeof(offsets));
}

// Register some meta-data with the meta-data manager. This is a no-op if the
// meta-data has already been registered.
void MetaDataManager::Register(MetaDataDescription *desc) {
  if (!is_finalized) {
    FineGrainedLocked locker(&next_description_id_lock);
    if (-1 == desc->id) {
      GRANARY_ASSERT(MAX_NUM_MANAGED_METADATAS > next_description_id);
      desc->id = next_description_id++;
    }
    descriptions[desc->id] = desc;
  }
}

// Allocate some meta-data. If the manager hasn't been finalized then this
// returns `nullptr`.
BlockMetaData *MetaDataManager::Allocate(void) {
  if (GRANARY_UNLIKELY(!is_finalized)) {
    Finalize();
    InitAllocator();
  }
  auto meta = new (allocator->Allocate()) BlockMetaData(this);
  VALGRIND_MALLOCLIKE_BLOCK(meta, size, 0, 0);
  return meta;
}

// Free some meta-data. This is a no-op if the manager hasn't been finalized.
void MetaDataManager::Free(BlockMetaData *meta) {
  GRANARY_ASSERT(is_finalized);
  GRANARY_ASSERT(this == meta->manager);
  allocator->Free(meta);
  VALGRIND_FREELIKE_BLOCK(meta, size);
}


// Finalizes the meta-data structures, which determines the runtime layout
// of the packed meta-data structure.
void MetaDataManager::Finalize(void) {
  is_finalized = true;
  for (auto desc : descriptions) {
    if (desc) {
      size += GRANARY_ALIGN_FACTOR(size, desc->align);
      offsets[desc->id] = size;
      size += desc->size;
    }
  }
  size += GRANARY_ALIGN_FACTOR(size, alignof(BlockMetaData));
}

// Initialize the allocator for meta-data managed by this manager.
void MetaDataManager::InitAllocator(void) {
  auto offset = GRANARY_ALIGN_TO(sizeof(internal::SlabList), size);
  auto remaining_size = GRANARY_ARCH_PAGE_FRAME_SIZE - offset;
  auto max_num_allocs = remaining_size / size;
  allocator.Construct(max_num_allocs, offset, size, size);
}

}  // namespace granary
