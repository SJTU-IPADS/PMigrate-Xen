/*
 * include xen/mc_migration.h
 */
#ifndef __XEN_MC_MIGRATION_H__
#define __XEN_MC_MIGRATION_H__

#include <asm/atomic.h>

/*
 * batch size in multi-core migration
 * default value is 128M
 * TO_TEST set batch size into 4M
 */
#define BATCH_DEFAULT 128
#define BATCH_TEST 4
#define MC_DEFAULT_BATCH_SIZE (BATCH_TEST * 1024 * 1024)
#define MC_DEFAULT_BATCH_L1_LENGTH (MC_DEFAULT_BATCH_SIZE / (EPT_PAGETABLE_ENTRIES * 4 * 1024))

#define SLAVE_TRIGGER_THRESHOLD 16

struct sync_entry {
    mfn_t ept_page_mfn;
    int start;
    int len;
};

struct mc_migr_sync {
    atomic_t consume_size;
    atomic_t current_size;
    volatile int end;

    struct sync_entry entry_list[0];
};

struct mc_slave_data {
    struct mc_migr_sync *migration_sync;
    cpumask_t slave_cpumask;

    p2m_type_t ot;
    p2m_type_t nt;

    atomic_t slave_cnt;
};

#endif
