/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifndef XCCL_UCS_H_
#define XCCL_UCS_H_

#include <inttypes.h>
#include <ucs/arch/x86_64/bitops.h>

#define ucs_ilog2(_n)                   \
(                                       \
    __builtin_constant_p(_n) ? (        \
             (_n) < 1 ? 0 :             \
             (_n) & (1ULL << 63) ? 63 : \
             (_n) & (1ULL << 62) ? 62 : \
             (_n) & (1ULL << 61) ? 61 : \
             (_n) & (1ULL << 60) ? 60 : \
             (_n) & (1ULL << 59) ? 59 : \
             (_n) & (1ULL << 58) ? 58 : \
             (_n) & (1ULL << 57) ? 57 : \
             (_n) & (1ULL << 56) ? 56 : \
             (_n) & (1ULL << 55) ? 55 : \
             (_n) & (1ULL << 54) ? 54 : \
             (_n) & (1ULL << 53) ? 53 : \
             (_n) & (1ULL << 52) ? 52 : \
             (_n) & (1ULL << 51) ? 51 : \
             (_n) & (1ULL << 50) ? 50 : \
             (_n) & (1ULL << 49) ? 49 : \
             (_n) & (1ULL << 48) ? 48 : \
             (_n) & (1ULL << 47) ? 47 : \
             (_n) & (1ULL << 46) ? 46 : \
             (_n) & (1ULL << 45) ? 45 : \
             (_n) & (1ULL << 44) ? 44 : \
             (_n) & (1ULL << 43) ? 43 : \
             (_n) & (1ULL << 42) ? 42 : \
             (_n) & (1ULL << 41) ? 41 : \
             (_n) & (1ULL << 40) ? 40 : \
             (_n) & (1ULL << 39) ? 39 : \
             (_n) & (1ULL << 38) ? 38 : \
             (_n) & (1ULL << 37) ? 37 : \
             (_n) & (1ULL << 36) ? 36 : \
             (_n) & (1ULL << 35) ? 35 : \
             (_n) & (1ULL << 34) ? 34 : \
             (_n) & (1ULL << 33) ? 33 : \
             (_n) & (1ULL << 32) ? 32 : \
             (_n) & (1ULL << 31) ? 31 : \
             (_n) & (1ULL << 30) ? 30 : \
             (_n) & (1ULL << 29) ? 29 : \
             (_n) & (1ULL << 28) ? 28 : \
             (_n) & (1ULL << 27) ? 27 : \
             (_n) & (1ULL << 26) ? 26 : \
             (_n) & (1ULL << 25) ? 25 : \
             (_n) & (1ULL << 24) ? 24 : \
             (_n) & (1ULL << 23) ? 23 : \
             (_n) & (1ULL << 22) ? 22 : \
             (_n) & (1ULL << 21) ? 21 : \
             (_n) & (1ULL << 20) ? 20 : \
             (_n) & (1ULL << 19) ? 19 : \
             (_n) & (1ULL << 18) ? 18 : \
             (_n) & (1ULL << 17) ? 17 : \
             (_n) & (1ULL << 16) ? 16 : \
             (_n) & (1ULL << 15) ? 15 : \
             (_n) & (1ULL << 14) ? 14 : \
             (_n) & (1ULL << 13) ? 13 : \
             (_n) & (1ULL << 12) ? 12 : \
             (_n) & (1ULL << 11) ? 11 : \
             (_n) & (1ULL << 10) ? 10 : \
             (_n) & (1ULL <<  9) ?  9 : \
             (_n) & (1ULL <<  8) ?  8 : \
             (_n) & (1ULL <<  7) ?  7 : \
             (_n) & (1ULL <<  6) ?  6 : \
             (_n) & (1ULL <<  5) ?  5 : \
             (_n) & (1ULL <<  4) ?  4 : \
             (_n) & (1ULL <<  3) ?  3 : \
             (_n) & (1ULL <<  2) ?  2 : \
             (_n) & (1ULL <<  1) ?  1 : \
             (_n) & (1ULL <<  0) ?  0 : \
             0                          \
                                 ) :    \
    (sizeof(_n) <= 4) ?                 \
    __ucs_ilog2_u32((uint32_t)(_n)) :   \
    __ucs_ilog2_u64((uint64_t)(_n))     \
)

/* Returns the number of 1-bits in x */
#define ucs_popcount(_n) \
    ((sizeof(_n) <= 4) ? __builtin_popcount((uint32_t)(_n)) : __builtin_popcountl(_n))

#endif
