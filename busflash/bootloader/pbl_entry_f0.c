/*
 * PBL target entry points + header (placed first in the PBL page).
 * SPDX-License-Identifier: MPL-2.0
 */
#include <stdint.h>
#include "pbl.h"

#define STR_(x) #x
#define STR(x) STR_(x)
_Static_assert(PBL_RAM_TOP == 0x20001000u, "keep PBL_RAM_TOP_ASM in sync");
#define PBL_RAM_TOP_ASM 0x20001000

void pbl_start(uint32_t addr, uint32_t from_reset) __attribute__((noreturn));
void pbl_entry_app(uint32_t addr);
void pbl_entry_reset(void);

struct pbl_header {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t entry_app;
    uint32_t entry_reset;
};

__attribute__((section(".pbl_header"), used))
const struct pbl_header pbl_header = {
    PBL_HEADER_MAGIC, PBL_VERSION, 0,
    (uint32_t)&pbl_entry_app, (uint32_t)&pbl_entry_reset,
};

/* Called by the application with IRQs disabled, r0 = bus address. */
__attribute__((naked)) void pbl_entry_app(__attribute__((unused)) uint32_t addr)
{
    __asm volatile (
        "ldr  r1, =" STR(PBL_RAM_TOP_ASM) "\n"
        "msr  msp, r1      \n"
        "movs r1, #0       \n"
        "b    pbl_start    \n"
        ".ltorg            \n");
}

/* Reset vector of the recovery stub in page 0.  The core has already loaded
 * SP = stub word 0 = PBL_RAM_TOP; after reset no interrupt source is enabled. */
__attribute__((naked)) void pbl_entry_reset(void)
{
    __asm volatile (
        "movs r0, #0       \n"
        "movs r1, #1       \n"
        "b    pbl_start    \n");
}
