#ifndef __SMMTT_DEFS_H__
#define __SMMTT_DEFS_H__

/* Parameterize based on build and bitness */

#if defined(SMMTT_QEMU)

#include "linux/kvm.h"

#if defined(TARGET_RISCV32)
#define __SMMTT32
#elif defined(TARGET_RISCV64)
#define __SMMTT64
#else
#error "Unknown target for QEMU"
#endif

#elif defined(SMMTT_OPENSBI)

#include <sbi/sbi_const.h>

#if __riscv_xlen == 32
#define __SMMTT32
#elif __riscv_xlen == 64
#define __SMMTT64
#else
#error "Unknown xlen for OpenSBI"
#endif

#else
#error "Must be included from QEMU or OpenSBI"
#endif

/* SMMTT Modes */

typedef enum {
    SMMTT_BARE = 0,
#if defined(TARGET_RISCV32)
    SMMTT_34,
    SMMTT_34_rw
#elif defined(TARGET_RISCV64)
    SMMTT_46,
    SMMTT_46_rw,
    SMMTT_56,
    SMMTT_56_rw
#else
// We error only here in this case, since otherwise we would need to do
// this preprocessor check everywhere.
#error "SMMTT is for RISC-V only"
#endif
} smmtt_mode_t;

/* MTTP CSR */

#define MTTP32_MODE_MASK	    _UL(0xC0000000)
#define MTTP32_SDID_MASK	    _UL(0x3F000000)
#define MTTP32_PPN_MASK	        _UL(0x003FFFFF)

#define MTTP32_MODE_SHIFT		30
#define MTTP32_SDID_SHIFT		24

#define MTTP64_MODE_MASK	    _ULL(0xF000000000000000)
#define MTTP64_SDID_MASK	    _ULL(0x0FC0000000000000)
#define MTTP64_PPN_MASK		    _ULL(0x00000FFFFFFFFFFF)

#define MTTP64_MODE_SHIFT		60
#define MTTP64_SDID_SHIFT		54

#if defined(__SMMTT32)
#define MTTP_MODE_MASK     MTTP32_MODE_MASK
#define MTTP_SDID_MASK     MTTP32_SDID_MASK
#define MTTP_PPN_MASK      MTTP32_PPN_MASK

#define MTTP_MODE_SHIFT    MTTP32_MODE_SHIFT
#define MTTP_SDID_SHIFT    MTTP32_SDID_SHIFT
#else
#define MTTP_MODE_MASK     MTTP64_MODE_MASK
#define MTTP_SDID_MASK     MTTP64_SDID_MASK
#define MTTP_PPN_MASK      MTTP64_PPN_MASK

#define MTTP_MODE_SHIFT    MTTP64_MODE_SHIFT
#define MTTP_SDID_SHIFT    MTTP64_SDID_SHIFT
#endif

/* MTT Tables */

// Masks

#if defined(__SMMTT32)

#define SPA_MTTL0_MASK     _ULL(0x000007000)
#define SPA_MTTL1_MASK     _ULL(0x001ff8000)
#define SPA_XM_OFFS _ULL(0x001C00000)
#define SPA_MTTL2_MASK     _ULL(0x3fe000000)

#else

#define SPA_MTTL3_MASK        _ULL(0xffc00000000000)

#define SPA_MTTL2_MASK        _ULL(0x003ffffc000000)
#define SPA_MTTL1_MASK        _ULL(0x00000003fe0000)
#define SPA_MTTL0_MASK        _ULL(0x0000000001f000)

#define SPA_MTTL2_RW_MASK     _ULL(0x003ffffe000000)
#define SPA_MTTL1_RW_MASK     _ULL(0x00000001ff0000)
#define SPA_MTTL0_RW_MASK     _ULL(0x0000000000f000)

#define SPA_2M_OFFS _ULL(0x00000001e00000)

#endif

// MTT table restriction types
typedef enum {
    SMMTT_TYPE_1G_DISALLOW = 0b00,
    SMMTT_TYPE_1G_ALLOW   = 0b01,
    SMMTT_TYPE_MTT_L1_DIR  = 0b10,
    SMMTT_TYPE_2M_PAGES    = 0b11
} smmtt_type_t;

// Types
typedef enum {
    SMMTT_TYPE_RW_DISALLOW_1G = 0b0000,
    SMMTT_TYPE_RW_ALLOW_R_1G  = 0b0001,
    SMMTT_TYPE_RW_ALLOW_RW_1G = 0b0011,
    SMMTT_TYPE_RW_MTT_L1_DIR  = 0b0100,
    SMMTT_TYPE_RW_2M_PAGES    = 0b0111
} smmtt_rw_type_t;

// Permissions

typedef enum {
    SMMTT_PERMS_2M_PAGES_DISALLOWED = 0b00,
    SMMTT_PERMS_2M_PAGES_ALLOW_RX   = 0b01,
    SMMTT_PERMS_2M_PAGES_ALLOW_RWX  = 0b11,
} smmtt_perms_xm_pages_t;

typedef enum {
    SMMTT_PERMS_MTT_L1_DIR_DISALLOWED   = 0b00,
    SMMTT_PERMS_MTT_L1_DIR_ALLOW_RX     = 0b01,
    SMMTT_PERMS_MTT_L1_DIR_ALLOW_RW     = 0b10,
    SMMTT_PERMS_MTT_L1_DIR_ALLOW_RWX    = 0b11,
} smmtt_perms_mtt_l1_dir_t;

#define MTT_PERMS_MASK  _ULL(0b11)
#define MTT_PERMS_BITS  (2)

#define MTT_PERM_FIELD(idx) \
    MTT_PERMS_MASK << (MTT_PERMS_BITS * (idx))

// Entries

#if defined(__SMMTT32)

typedef struct {
    uint32_t info : 22;
    uint32_t type : 3;
    uint32_t zero : 7;
} mttl2_entry_t;

typedef uint32_t mttl1_entry_t;

typedef union {
    uint32_t raw;
    mttl2_entry_t mttl2;
    mttl1_entry_t mttl1;
} smmtt_mtt_entry_t;

#else

typedef struct {
    uint64_t mttl2_ppn : 44;
    uint64_t zero : 20;
} mttl3_entry_t;

typedef struct {
    uint64_t info : 44;
    uint64_t type : 4;
    uint64_t zero : 16;
} mttl2_entry_rw_t;

typedef struct {
    uint64_t info : 44;
    uint64_t type : 2;
    uint64_t zero : 18;
} mttl2_entry_t;

typedef uint64_t mttl1_entry_t;

typedef union {
    uint64_t raw;
    mttl3_entry_t mttl3;
    mttl2_entry_t mttl2;
    mttl2_entry_rw_t mttl2_rw;
    mttl1_entry_t mttl1;
} smmtt_mtt_entry_t;

#endif



#endif // __SMMTT_DEFS_H__