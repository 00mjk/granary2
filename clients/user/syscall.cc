/* Copyright 2012-2014 Peter Goodman, all rights reserved. */
/*
 * syscall.cc
 *
 *  Created on: Sep 8, 2014
 *      Author: Peter Goodman
 */

#include "clients/user/syscall.h"

using namespace granary;

namespace {

// Linked list of system call hooks.
class SystemCallClosure {
 public:
  SystemCallClosure(SystemCallHook *callback_, void *data_,
                    void (*delete_data_)(void *))
      : next(nullptr),
        callback(callback_),
        data(data_),
        delete_data(delete_data_) {}

  ~SystemCallClosure(void) {
    if (delete_data) {
      delete_data(data);
    }
  }

  GRANARY_DEFINE_NEW_ALLOCATOR(SystemCallClosure, {
    SHARED = true,
    ALIGNMENT = 32
  })

  SystemCallClosure *next;
  SystemCallHook *callback;
  void *data;
  void (*delete_data)(void *);
};

typedef LinkedListIterator<SystemCallClosure> SystemCallClosureIterator;

static ReaderWriterLock syscall_hooks_lock;
static SystemCallClosure *entry_hooks = nullptr;
static SystemCallClosure **next_entry_hook = &entry_hooks;
static SystemCallClosure *exit_hooks = nullptr;
static SystemCallClosure **next_exit_hook = &exit_hooks;

// Frees a list of system call closures.
static void FreeHookList(SystemCallClosure *closure) {
  SystemCallClosure *next_closure(nullptr);
  for (; closure; closure = next_closure) {
    next_closure = closure->next;
    delete closure;
  }
}

}  // namespace

// Deletes all hooks and restores the syscall hooking system to its original
// state. This is done during `User::Exit`.
void RemoveAllHooks(void) {
  WriteLocked locker(&syscall_hooks_lock);
  FreeHookList(entry_hooks);
  FreeHookList(exit_hooks);
  entry_hooks = nullptr;
  next_entry_hook = &entry_hooks;
  exit_hooks = nullptr;
  next_exit_hook = &exit_hooks;
}

// Handle a system call entrypoint.
void HookSystemCallEntry(arch::MachineContext *context) {
  ReadLocked locker(&syscall_hooks_lock);
  SystemCallContext syscall_context(context);
  for (auto closure : SystemCallClosureIterator(entry_hooks)) {
    closure->callback(syscall_context, closure->data);
  }
}

// Handle a system call exit.
void HookSystemCallExit(arch::MachineContext *context) {
  ReadLocked locker(&syscall_hooks_lock);
  SystemCallContext syscall_context(context);
  for (auto closure : SystemCallClosureIterator(exit_hooks)) {
    closure->callback(syscall_context, closure->data);
  }
}

// Register a function to be called before a system call is made.
void AddSystemCallEntryFunction(SystemCallHook *callback,
                                void *data,
                                CleanUpData *delete_data) {
  auto closure = new SystemCallClosure(callback, data, delete_data);
  WriteLocked locker(&syscall_hooks_lock);
  *next_entry_hook = closure;
  next_entry_hook = &(closure->next);
}

// Register a function to be called after a system call is made.
void AddSystemCallExitFunction(SystemCallHook *callback,
                               void *data,
                               CleanUpData *delete_data) {
  auto closure = new SystemCallClosure(callback, data, delete_data);
  WriteLocked locker(&syscall_hooks_lock);
  *next_exit_hook = closure;
  next_exit_hook = &(closure->next);
}
