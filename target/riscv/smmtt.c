
#include "smmtt.h"

#include "qemu/log.h"
#include "qapi/error.h"
#include "trace.h"
#include "exec/exec-all.h"
#include "smmtt_defs.h"

static const unsigned long long masks_rw[] = {
        SPA_MTTL3_MASK, SPA_MTTL2_RW_MASK, SPA_MTTL1_MASK, SPA_MTTL0_MASK
};

static const unsigned long long masks[] = {
        SPA_MTTL3_MASK, SPA_MTTL2_MASK, SPA_MTTL1_MASK, SPA_MTTL0_MASK
};

// MTT table shifts
#define MTTL3_SHIFT     (46)

#define MTTL2_RW_SHIFT  (25)
#define MTTL1_RW_SHIFT  (16)

#define MTTL2_SHIFT     (26)
#define MTTL1_SHIFT     (17)

static const unsigned int shifts_rw[] = {
        MTTL3_SHIFT, MTTL2_RW_SHIFT, MTTL1_RW_SHIFT, 0
};

static const unsigned int shifts[] = {
        MTTL3_SHIFT, MTTL2_SHIFT, MTTL1_SHIFT, 0
};

#define MTTL2_2M_PAGES_SHIFT    (21) /* 2 megabytes */
#define MTTL2_RW_PAGES_MASK     (0b11)
#define MTTL2_PAGES_MASK        (0b1)

/*
 * Internal helpers
 */

static int smmtt_decode_mttp(CPURISCVState *env, bool *rw, int *levels) {
    smmtt_mode_t smmtt_mode = get_field(env->mttp, MTTP_MODE_MASK);

    switch(smmtt_mode) {
        case SMMTT_BARE:
            *levels = -1;
            break;

            // Determine if rw
#if defined(TARGET_RISCV32)
        case SMMTT_34_rw:
#elif defined(TARGET_RISCV64)
        case SMMTT_46_rw:
#endif
            *rw = true;
#if defined(TARGET_RISCV32)
        // fall through
        case SMMTT_34:
#elif defined(TARGET_RISCV64)
        // fall through
        case SMMTT_46:
#endif
            *levels = 2;
            break;

            // Handle 56 bit lookups (3 stage)
#if defined(TARGET_RISCV64)
        case SMMTT_56_rw:
            *rw = true;
        // fall through
        case SMMTT_56:
            *levels = 3;
            break;
#endif
        default:
            return -1;
    }

    return 0;
}

static int smmtt_decode_mttl2(hwaddr addr, bool rw, smmtt_mtt_entry_t entry,
                              int *privs, hwaddr *next, bool *done) {
    smmtt_type_t type;
    smmtt_type_rw_t type_rw;
    target_ulong idx;

    *done = false;
    if(rw) {
        if(entry.zero != 0) {
            *done = true;
            return 0;
        }

        type_rw = (smmtt_type_rw_t) entry.type;
        switch(type_rw) {
            case SMMTT_TYPE_RW_DISALLOW_1G:
                *privs = 0;
                *done = true;
                break;

            case SMMTT_TYPE_RW_ALLOW_RW_1G:
                *privs |= PAGE_WRITE;
                // fall through
            case SMMTT_TYPE_RW_ALLOW_R_1G:
                *privs |= PAGE_READ;
                *done = true;
                break;

            case SMMTT_TYPE_RW_2M_PAGES:
                if(entry.info >> 32 != 0) {
                    return -1;
                }

                idx = (addr & SPA_MTTL2_RW_MASK) >> MTTL2_2M_PAGES_SHIFT;
                switch(get_field(entry.info, MTTL2_RW_PAGES_MASK << idx)) {
                    case SMMTT_PERMS_2M_PAGES_DISALLOWED:
                        *privs = 0;
                        break;

                    case SMMTT_PERMS_2M_PAGES_ALLOW_RWX:
                        *privs |= PAGE_WRITE;
                        // fall through
                    case SMMTT_PERMS_2M_PAGES_ALLOW_RX:
                        *privs |= PAGE_READ;
                        break;

                    default:
                        return -1;
                }

                *done = true;
                break;

            case SMMTT_TYPE_RW_MTT_L1_DIR:
                *next = entry.info << PGSHIFT;
                *done = false;
                break;

            default:
                return -1;
        }
    } else {
        if(entry.mttl2.zero != 0) {
            return false;
        }

        type = (smmtt_type_t) entry.mttl2.type;
        switch(type) {
            case SMMTT_TYPE_1G_DISALLOW:
                *privs = 0;
                *done = true;
                break;
            case SMMTT_TYPE_1G_ALLOW:
                *privs = (PAGE_READ | PAGE_WRITE | PAGE_EXEC);
                *done = true;
                break;

            case SMMTT_TYPE_2M_PAGES:
                if(entry.mttl2.info >> 32 != 0) {
                    return -1;
                }

                idx = (addr & SPA_MTTL2_MASK) >> MTTL2_2M_PAGES_SHIFT;
                switch(get_field(entry.mttl2.info, MTTL2_PAGES_MASK << idx)) {
                    case 0b0:
                        *privs = 0;
                        break;

                    case 0b1:
                        *privs = (PAGE_READ | PAGE_WRITE | PAGE_EXEC);
                        break;

                    default:
                        return -1;
                }

                *done = true;
                break;

            case SMMTT_TYPE_MTT_L1_DIR:
                *next = entry.mttl2.info << PGSHIFT;
                *done = false;
                break;
        }
    }

    return 0;
}

/*
 * Public Interface
 */


bool smmtt_hart_has_privs(CPURISCVState *env, hwaddr addr,
                        target_ulong size, int privs,
                        int *allowed_privs, target_ulong mode) {
    // SMMTT configuration
    bool rw = false;
    int levels = 0;
    smmtt_mtt_entry_t entry = {
            .raw = 0,
    };

    const unsigned int *sh;
    const unsigned long long *msk;

    // Results and indices
    bool done = false;
    int ret;
    MemTxResult res;
    target_ulong idx;
    hwaddr curr;

    CPUState *cs = env_cpu(env);
    if(!riscv_cpu_cfg(env)->ext_smmtt || (mode == PRV_M)) {
        *allowed_privs = (PAGE_READ | PAGE_WRITE | PAGE_EXEC);
        return true;
    }

    ret = smmtt_decode_mttp(env, &rw, &levels);
    if(ret < 0) {
        return false;
    }

    if(levels == -1) {
        // This must be SMMTT_BARE, so SMMTT will allow accesses here
        *allowed_privs = (PAGE_READ | PAGE_WRITE | PAGE_EXEC);
    } else {
        // Initialize allowed privileges to 0 and discover
        // what's allowed on the way.
        *allowed_privs = 0;
    }

    sh = rw ? shifts_rw : shifts;
    msk = rw ? masks_rw : masks;
    curr = (hwaddr) get_field(env->mttp, MTTP_PPN_MASK) << PGSHIFT;

    for(; levels >= 0 && !done; levels--) {
        idx = (addr >> sh[levels]) & msk[levels];
        if(levels != 0) {
            // Fetch an entry
            curr = curr + idx * 8;
            entry.raw = address_space_ldq(cs->as, curr, MEMTXATTRS_UNSPECIFIED, &res);
        }

        switch (levels) {
            case 3:
                if(entry.mttl3.zero != 0) {
                    return false;
                }

                curr = entry.mttl3.mttl2_ppn << PGSHIFT;
                break;

            case 2:
                ret = smmtt_decode_mttl2(addr, rw, entry, allowed_privs,
                                         &curr, &done);
                if(ret < 0) {
                    return false;
                }
                break;

            case 1:
                // Do nothing here besides translate, and preserve
                // entry for the next go around
                break;

            case 0:
                if(rw) {
                    switch(get_field(entry.mttl1, 0b1111 << idx)) {
                        case 0b0000:
                            *allowed_privs = 0;
                            break;

                        case 0b0011:
                            *allowed_privs |= (PROT_WRITE);
                            // fall through
                        case 0b0001:
                            *allowed_privs |= (PROT_READ);
                            break;

                        default:
                            return false;
                    }
                } else {
                    switch(get_field(entry.mttl1, 0b11 << idx)) {
                        case 0b00:
                            *allowed_privs = 0;
                            break;

                        case 0b11:
                            *allowed_privs = (PROT_READ | PROT_WRITE | PROT_EXEC);
                            break;

                        default:
                            return false;
                    }
                }
                break;

            default:
                return false;

        }
    }

    // ASSUMPTION: we assume that read implies execute, and leave it up to other
    // parts of the memory hierarchy to indicate execute permissions.
    if(*allowed_privs & PROT_READ) {
        *allowed_privs |= PROT_EXEC;
    }

    return (privs & *allowed_privs) == privs;
}