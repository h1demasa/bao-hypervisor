/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

#include <plic.h>
#include <interrupts.h>
#include <cpu.h>
#include <fences.h>

size_t PLIC_IMPL_INTERRUPTS;

volatile struct plic_global_hw* plic_global;

volatile struct plic_hart_hw* plic_hart;

static size_t plic_scan_max_int()
{
    size_t res = 0;
    for (size_t i = 1; i < PLIC_MAX_INTERRUPTS; i++) {
        plic_global->prio[i] = ~0U;
        if (plic_global->prio[i] == 0) {
            res = i - 1;
            break;
        }
        plic_global->prio[i] = 0;
    }
    return res;
}

void plic_init(void)
{
    /** Maps PLIC device */
    plic_global = (void*)mem_alloc_map_dev(&cpu()->as, SEC_HYP_GLOBAL, INVALID_VA,
        platform.arch.irqc.plic.base, NUM_PAGES(sizeof(struct plic_global_hw)));

    plic_hart = (void*)mem_alloc_map_dev(&cpu()->as, SEC_HYP_GLOBAL, INVALID_VA,
        platform.arch.irqc.plic.base + HART_REG_OFF,
        NUM_PAGES(sizeof(struct plic_hart_hw) * IRQC_HART_INST));

    /** Ensure that instructions after fence have the PLIC fully mapped */
    fence_sync();

    PLIC_IMPL_INTERRUPTS = plic_scan_max_int();

    for (size_t i = 0; i <= PLIC_IMPL_INTERRUPTS; i++) {
        plic_global->prio[i] = 0;
    }

    for (size_t i = 0; i < PLIC_PLAT_CNTXT_NUM; i++) {
        for (size_t j = 0; j < PLIC_NUM_ENBL_REGS; j++) {
            plic_global->enbl[i][j] = 0;
        }
    }
}

void plic_cpu_init(void)
{
    cpu()->arch.plic_cntxt =
        (unsigned)plic_plat_cntxt_to_id((struct plic_cntxt){ cpu()->id, PRIV_S });
    plic_hart[cpu()->arch.plic_cntxt].threshold = 0;
}

static bool plic_cntxt_valid(size_t cntxt_id)
{
    struct plic_cntxt cntxt = plic_plat_id_to_cntxt(cntxt_id);
    return (cntxt_id < PLIC_PLAT_CNTXT_NUM) && (cntxt.mode <= PRIV_S);
}

void plic_set_enbl(size_t cntxt, irqid_t int_id, bool en)
{
    size_t reg_ind = int_id / (sizeof(uint32_t) * 8);
    uint32_t mask = 1U << (int_id % (sizeof(uint32_t) * 8));

    if (int_id <= PLIC_IMPL_INTERRUPTS && plic_cntxt_valid(cntxt)) {
        if (en) {
            plic_global->enbl[cntxt][reg_ind] |= mask;
        } else {
            plic_global->enbl[cntxt][reg_ind] &= ~mask;
        }
    }
}

bool plic_get_enbl(size_t cntxt, irqid_t int_id)
{
    size_t reg_ind = int_id / (sizeof(uint32_t) * 8);
    uint32_t mask = 1U << (int_id % (sizeof(uint32_t) * 8));

    if (int_id <= PLIC_IMPL_INTERRUPTS && plic_cntxt_valid(cntxt)) {
        return plic_global->enbl[cntxt][reg_ind] & mask;
    } else {
        return false;
    }
}

void plic_set_prio(irqid_t int_id, uint32_t prio)
{
    if (int_id <= PLIC_IMPL_INTERRUPTS) {
        plic_global->prio[int_id] = prio;
    }
}

uint32_t plic_get_prio(irqid_t int_id)
{
    if (int_id <= PLIC_IMPL_INTERRUPTS) {
        return plic_global->prio[int_id];
    } else {
        return 0;
    }
}

bool plic_get_pend(irqid_t int_id)
{
    size_t reg_ind = (size_t)(int_id / 32);
    uint32_t mask = (1U << (int_id % 32));

    if (int_id <= PLIC_IMPL_INTERRUPTS) {
        return plic_global->pend[reg_ind] & mask;
    } else {
        return false;
    }
}

void plic_set_threshold(size_t cntxt, uint32_t threshold)
{
    if (plic_cntxt_valid(cntxt)) {
        plic_hart[cntxt].threshold = threshold;
    }
}

uint32_t plic_get_threshold(unsigned cntxt)
{
    uint32_t threshold = 0;
    if (plic_cntxt_valid(cntxt)) {
        threshold = plic_hart[cntxt].threshold;
    }
    return threshold;
}

void plic_handle(void)
{
    uint32_t id = plic_hart[cpu()->arch.plic_cntxt].claim;

    if (id != 0) {
        cpu()->arch.handling_irq.external_id = id;
        enum irq_res res = interrupts_handle(id);
        if (res == HANDLED_BY_HYP) {
            plic_hart[cpu()->arch.plic_cntxt].complete = id;
        }
    }
}

/**
 * Context organization is spec-out by the vendor, this is the default mapping found in sifive's
 * plic.
 */

__attribute__((weak)) ssize_t plic_plat_cntxt_to_id(struct plic_cntxt cntxt)
{
    if (cntxt.mode != PRIV_M && cntxt.mode != PRIV_S) {
        return -1;
    }
    return (ssize_t)((cntxt.hart_id * 2) + (cntxt.mode == PRIV_M ? 0 : 1));
}

__attribute__((weak)) struct plic_cntxt plic_plat_id_to_cntxt(size_t id)
{
    struct plic_cntxt cntxt;
    if (id < PLIC_PLAT_CNTXT_NUM) {
        cntxt.hart_id = id / 2;
        cntxt.mode = (id % 2) == 0 ? PRIV_M : PRIV_S;
    } else {
        return (struct plic_cntxt){ INVALID_CPUID, 0 };
    }
    return cntxt;
}
