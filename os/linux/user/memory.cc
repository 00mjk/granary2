/* Copyright 2014 Peter Goodman, all rights reserved. */

#include "arch/base.h"
#include "granary/base/base.h"

#include "os/memory.h"

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20

extern "C" {

extern void *mmap(void *__addr, size_t __len, int __prot, int __flags,
                  int __fd, long __offset);
extern int munmap(void *__addr, size_t __len);
extern int mprotect(void *__addr, size_t __len, int __prot);
extern int mlock(const void *__addr, size_t __len);

}  // extern C
namespace granary {
namespace os {

// Initialize the Granary heap.
void InitHeap(void) {}

// Allocates `num` number of pages from the OS with `MEMORY_READ_WRITE`
// protection.
void *AllocatePages(int num, MemoryIntent intent) {
  auto prot = PROT_READ | PROT_WRITE;
  if (MemoryIntent::EXECUTABLE == intent) {
    prot |= PROT_EXEC;
  }
  auto num_bytes = static_cast<size_t>(arch::PAGE_SIZE_BYTES * num);
  auto ret = mmap(nullptr, num_bytes, prot, MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
  if (MemoryIntent::EXECUTABLE == intent) {
    mlock(ret, num_bytes);
  }
  return ret;
}

// Frees `num` pages back to the OS.
void FreePages(void *addr, int num, MemoryIntent) {
  munmap(addr, static_cast<size_t>(arch::PAGE_SIZE_BYTES * num));
}

// Changes the memory protection of some pages.
void ProtectPages(void *addr, int num, MemoryProtection prot) {
  int prot_bits(0);
  if (MemoryProtection::EXECUTABLE == prot) {
    prot_bits = PROT_EXEC | PROT_READ | PROT_WRITE;
  } else if (MemoryProtection::READ_ONLY == prot) {
    prot_bits = PROT_READ;
  } else if (MemoryProtection::READ_WRITE == prot) {
    prot_bits = PROT_READ | PROT_WRITE;
  } else {
    prot_bits = 0; //  MEMORY_INACCESSIBLE
  }
  mprotect(
      addr,
      static_cast<size_t>(arch::PAGE_SIZE_BYTES * num),
      prot_bits);
}

}  // namespace os
}  // namespace granary
