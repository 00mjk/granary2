/* Copyright 2014 Peter Goodman, all rights reserved. */

#include "arch/x86-64/asm/include.asm.inc"

START_FILE_INTEL

#ifdef GRANARY_WHERE_user

// A curious person might wonder: Why did I not prefix each of these functions
// with `granary_`, and instead went with the symbol versioning approach (by
// using `symver.map`). The answer is that
//
//      1)  I can achieve roughly the same effect with symbol versioning.
//      2)  I hope that applying static analysis tools to Granary's source code
//          will explicitly recognize the names of these `libc` system calls
//          and do specific checks based on their usage.
//
// A drawback of this approach is that the prefix approach with `granary_`
// makes it explicit that Granary has its own versions of each of these `libc`
// functions. Without this explicit prefix, a new developer of Granary might
// get confused into thinking that *any* `libc` function can be used.

DEFINE_FUNC(fstat)
    mov    r10,rcx
    mov    eax, 5  // `__NR_fstat`.
    syscall
    ret
END_FUNC(fstat)

DEFINE_FUNC(mmap)
    mov    r10,rcx
    mov    eax, 9  // `__NR_mmap`.
    syscall
    cmp    rax,0xfffffffffffff001
    jae    L(granary_mmap_error)
    ret
L(granary_mmap_error):
    or     rax,0xffffffffffffffff
    ret
END_FUNC(mmap)

DEFINE_FUNC(munmap)
    mov    eax, 11  // `__NR_munmap`.
    syscall
    cmp    rax,0xfffffffffffff001
    jae    L(granary_munmap_error)
    ret
L(granary_munmap_error):
    or     rax,0xffffffffffffffff
    ret
END_FUNC(munmap)

DEFINE_FUNC(mprotect)
    mov    eax, 10  // `__NR_mprotect`.
    syscall
    cmp    rax,0xfffffffffffff001
    jae    L(granary_mprotect_error)
    ret
L(granary_mprotect_error):
    or     rax,0xffffffffffffffff
    ret
END_FUNC(mprotect)

DEFINE_FUNC(mlock)
    mov    eax, 149  // `__NR_mlock`.
    syscall
    cmp    rax,0xfffffffffffff001
    jae    L(granary_mlock_error)
    ret
L(granary_mlock_error):
    or     rax,0xffffffffffffffff
    ret
END_FUNC(mlock)

DEFINE_FUNC(open)
    mov    eax, 2  // `__NR_open`.
    syscall
    cmp    rax,0xfffffffffffff001
    jae    L(granary_open_error)
    ret
L(granary_open_error):
    or     rax,0xffffffffffffffff
    ret
END_FUNC(open)

DEFINE_FUNC(close)
    mov    eax, 3  // `__NR_close`.
    syscall
    cmp    rax,0xfffffffffffff001
    jae    L(granary_close_error)
    ret
L(granary_close_error):
    or     rax,0xffffffffffffffff
    ret
END_FUNC(close)

DEFINE_FUNC(read)
    mov    eax, 0  // `__NR_read`.
    syscall
    cmp    rax,0xfffffffffffff001
    jae    L(granary_read_error)
    ret
L(granary_read_error):
    or     rax,0xffffffffffffffff
    ret
END_FUNC(read)

DEFINE_FUNC(write)
    mov    eax,1  // `__NR_write`.
    syscall
    cmp    rax,0xfffffffffffff001
    jae    L(granary_write_error)
    ret
L(granary_write_error):
    or     rax,0xffffffffffffffff
    ret
END_FUNC(write)

DEFINE_FUNC(getpid)
    mov    eax, 39  // `__NR_getpid`.
    syscall
    ret
END_FUNC(getpid)

DEFINE_FUNC(alarm)
    mov    eax, 37  // `__NR_alarm`.
    syscall
    ret
END_FUNC(alarm)

DEFINE_FUNC(setitimer)
    mov    eax, 38  // `__NR_setitimer`.
    syscall
    ret
END_FUNC(setitimer)

DEFINE_FUNC(rt_sigaction)
    mov     r10, rcx  // arg4, `sigsetsize`.
    jmp generic_sigaction
END_FUNC(rt_sigaction)

DEFINE_FUNC(generic_sigaction)
    mov     eax, 13  // `__NR_rt_sigaction`.
    syscall
    ret
END_FUNC(generic_sigaction)

DEFINE_FUNC(sigaltstack)
    mov     eax, 131  // `__NR_sigaltstack`.
    syscall
    ret
END_FUNC(sigaltstack)

DEFINE_FUNC(prctl)
  mov     r10, rcx  // arg4, `arg4`.
  mov     eax, 157  // `__NR_prctl`.
  syscall
  ret
END_FUNC(prctl)

DEFINE_FUNC(arch_prctl)
    mov     eax, 158  // `__NR_arch_prctl`.
    syscall
    ret
END_FUNC(arch_prctl)

// Thread exit.
DEFINE_FUNC(sys_exit)
    mov     eax, 60  // `__NR_exit`.
    syscall
END_FUNC(sys_exit)

// extern long sys_clone (
//      unsigned long clone_flags ,     RDI
//      char * newsp ,                  RSI
//      int * parent_tidptr ,           RDX
//      int * child_tidptr ,            RCX
//      int tls_val ,                   R8
//      void (*func)()                  R9
//
DEFINE_FUNC(sys_clone)
    mov     r10, rcx  // arg4, `child_tidptr`.
    mov     eax, 56  // `__NR_clone`.
    mov     [rsi], r9
    sub     r9, 8
    syscall
    test rax, rax
    jz L(return_to_child)

L(return_to_parent):
    ret

L(return_to_child):
    pop rax
    call rax
    jmp sys_exit
END_FUNC(sys_clone)

DEFINE_FUNC(sys_futex)
    mov     r10, rcx  // arg4, `utime`.
    mov     eax, 202  // `__NR_futex`.
    syscall
    ret
END_FUNC(sys_futex)

DEFINE_FUNC(ptrace)
    mov     r10, rcx  // arg4, `data`.
    mov     eax, 101  // `__NR_ptrace`.
    syscall
    ret
END_FUNC(ptrace)

DEFINE_FUNC(kill)
    mov     eax, 62  // `__NR_kill`.
    syscall
    ret
END_FUNC(ptrace)

DEFINE_FUNC(nanosleep)
    mov     eax, 35  // `__NR_nanosleep`.
    syscall
    ret
END_FUNC(nanosleep)

DEFINE_FUNC(sched_yield)
    mov     eax, 24  // `__NR_sched_yield`.
    syscall
    ret
END_FUNC(sched_yield)

DECLARE_FUNC(granary_exit)
DEFINE_INST_FUNC(exit_group_ok)  // Can be called by instrumentation code.
    xor     rdi, rdi
    jmp     exit_group
END_FUNC(exit_group_ok)

DEFINE_FUNC(exit)
    jmp exit_group
END_FUNC(exit)

DEFINE_FUNC(_exit)
    jmp exit_group
END_FUNC(_exit)

DEFINE_FUNC(_Exit)
    jmp exit_group
END_FUNC(_Exit)

DEFINE_FUNC(exit_group)
    push    rdi
    xor     rdi, rdi  // `ExitReason::EXIT_PROGRAM`.
    call    granary_exit
    pop rdi
    mov     eax, 231  // `__NR_exit_group`.
    syscall
END_FUNC(exit_group)

DEFINE_INST_FUNC(rt_sigreturn)
    mov     eax, 15  // `__NR_rt_sigreturn`.
    syscall
END_FUNC(rt_sigreturn)

#endif  // GRANARY_WHERE_user

END_FILE
