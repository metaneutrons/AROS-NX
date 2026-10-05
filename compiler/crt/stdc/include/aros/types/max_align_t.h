#ifndef _AROS_STDDEF_MAX_ALIGN_T_H
#define _AROS_STDDEF_MAX_ALIGN_T_H

/*
    Copyright © 2025, The AROS Development Team. All rights reserved.
    $Id$

    max_align_t type definition
*/

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Clang and GCC provide max_align_t from their compiler resource headers.
 * Publish both compiler guards when AROS owns the definition so that either
 * header order remains valid, and defer to an existing compiler definition.
 */
#if !defined(__CLANG_MAX_ALIGN_T_DEFINED) && !defined(_GCC_MAX_ALIGN_T)
#define __CLANG_MAX_ALIGN_T_DEFINED
#define _GCC_MAX_ALIGN_T
#ifdef __GNUC__
# define __AROS_MAX_ALIGN_ATTR(type) \
    __attribute__((__aligned__(__alignof__(type))))
#else
# define __AROS_MAX_ALIGN_ATTR(type)
#endif
typedef struct {
    long long __ll __AROS_MAX_ALIGN_ATTR(long long);
    long double __ld __AROS_MAX_ALIGN_ATTR(long double);
} max_align_t;
#undef __AROS_MAX_ALIGN_ATTR
#endif

#ifdef __cplusplus
}
#endif

#endif /* _AROS_STDDEF_MAX_ALIGN_T_H */
