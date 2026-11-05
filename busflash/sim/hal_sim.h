/*
 * PBL host-simulator HAL: the same pbl_core.c runs as a Linux process.
 *   flash  = 32 KB file, mmap(MAP_SHARED) -> survives kill -9 (power cut)
 *   bus    = stdin (bytes to the feeder) / stdout (bytes from the feeder)
 *   reset  = exit(42); the bus simulator then "reboots" the feeder
 * Models the target behaviour that matters for the protocol:
 *   - flash erase/program semantics (program only on erased halfwords)
 *   - CPU stall during erase / commit: bytes arriving meanwhile are lost
 *
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef PBL_HAL_SIM_H
#define PBL_HAL_SIM_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#define PBL_NORETURN __attribute__((noreturn))
#define PBL_ENTRY_RESET_ADDR 0x08007C21u    /* value written into the stub */

extern uint8_t *sim_flash;
static int sim_busy;

static void sim_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#define HAL_BUSY_MS(ms) (sim_sleep_ms(ms), sim_busy = 1)

/* drop every byte that arrived while the "CPU" was stalled */
static void sim_drain(void)
{
    struct pollfd p = { 0, POLLIN, 0 };
    uint8_t junk[256];
    while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN))
        if (read(0, junk, sizeof junk) <= 0)
            exit(0);
}
#define HAL_BUSY_DONE() do { if (sim_busy) { sim_drain(); sim_busy = 0; } } while (0)

static inline void hal_init(void) {}

static inline int hal_uart_getc(int block)
{
    struct pollfd p = { 0, POLLIN, 0 };
    int r = poll(&p, 1, block ? -1 : 4);
    if (r <= 0)
        return -1;
    uint8_t c;
    if (read(0, &c, 1) != 1)
        exit(0);                            /* bus closed */
    return c;
}

static inline void hal_uart_send(const uint8_t *p, uint32_t n)
{
    if (write(1, p, n) != (ssize_t)n)
        exit(1);
}

static inline uint32_t hal_flash_read8(uint32_t off) { return sim_flash[off]; }
static inline uint32_t hal_flash_read16(uint32_t off)
{
    return (uint32_t)sim_flash[off] | ((uint32_t)sim_flash[off + 1] << 8);
}

static void hal_flash_erase(uint32_t page)
{
    memset(&sim_flash[page * 1024u], 0xFF, 1024);
    sim_sleep_ms(25);
    sim_busy = 1;
}

static void hal_flash_prog16(uint32_t off, uint32_t v)
{
    if (hal_flash_read16(off) != 0xFFFFu && v != 0)
        return;                             /* PGERR: halfword not erased */
    sim_flash[off] = (uint8_t)v;
    sim_flash[off + 1] = (uint8_t)(v >> 8);
}

static inline PBL_NORETURN void hal_reset(void)
{
    msync(sim_flash, 32768, MS_SYNC);
    exit(42);
}

#endif
