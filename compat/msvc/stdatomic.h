/*
 * stdatomic.h shim for MSVC versions that lack C11 <stdatomic.h>.
 *
 * This file is placed on the include path BEFORE the system headers so that
 * #include <stdatomic.h> in third-party C code (e.g. QuickJS) picks it up
 * transparently.  It uses MSVC _Interlocked* intrinsics to provide the
 * subset of C11 atomics that QuickJS actually needs.
 *
 * C++ compilations simply skip this file (C++ has its own <atomic>).
 */
#ifndef _MSVC_STDATOMIC_SHIM_H
#define _MSVC_STDATOMIC_SHIM_H

#if defined(_MSC_VER) && !defined(__cplusplus)

#include <intrin.h>

#ifndef _Atomic
#define _Atomic volatile
#endif

#define atomic_fetch_add(obj, arg) \
    _InterlockedExchangeAdd((volatile long *)(obj), (long)(arg))

#define atomic_fetch_sub(obj, arg) \
    _InterlockedExchangeAdd((volatile long *)(obj), -(long)(arg))

#define atomic_fetch_or(obj, arg) \
    _InterlockedOr((volatile long *)(obj), (long)(arg))

#define atomic_fetch_xor(obj, arg) \
    _InterlockedXor((volatile long *)(obj), (long)(arg))

#define atomic_fetch_and(obj, arg) \
    _InterlockedAnd((volatile long *)(obj), (long)(arg))

#define atomic_exchange(obj, desired) \
    _InterlockedExchange((volatile long *)(obj), (long)(desired))

#define atomic_load(obj) \
    (*(volatile long *)(obj))

#define atomic_store(obj, desired) \
    _InterlockedExchange((volatile long *)(obj), (long)(desired))

static __forceinline int
atomic_compare_exchange_strong(volatile long *obj,
                               long *expected, long desired)
{
    long old = _InterlockedCompareExchange(obj, desired, *expected);
    if (old == *expected)
        return 1;
    *expected = old;
    return 0;
}

#endif /* _MSC_VER && !__cplusplus */

#endif /* _MSVC_STDATOMIC_SHIM_H */
