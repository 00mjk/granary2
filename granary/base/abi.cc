/* Copyright 2014 Peter Goodman, all rights reserved. */
#ifndef GRANARY_TARGET_test
extern "C" {

// C++ exception handling.
void __cxa_throw(void) { __builtin_trap(); }
void __cxa_allocate_exception(void) { __builtin_trap(); }
void __cxa_free_exception(void) { __builtin_trap(); }
void __cxa_atexit(void) {}

// std::__throw_bad_function_call.
void _ZSt25__throw_bad_function_callv(void) { __builtin_trap(); }

// C++ virtual functions.
void __cxa_pure_virtual(void) { __builtin_trap(); }

// C++ allocators.
void _ZdlPv(void) { __builtin_trap(); }  // `operator new`.
void _Znam(void) { __builtin_trap(); }  // `operator new[]`.
void _Znwm(void) { __builtin_trap(); }  // `operator delete`.
void _ZdaPv(void) { __builtin_trap(); } // `operator delete[]`.

// Used by XED.
void fprintf(void) { __builtin_trap(); }
void strncat(void) { __builtin_trap(); }
void abort(void) { __builtin_trap(); }

void *__dso_handle = nullptr;
void *stderr = nullptr;
void *stdout = nullptr;

}  // extern C
#endif  // GRANARY_TARGET_test
