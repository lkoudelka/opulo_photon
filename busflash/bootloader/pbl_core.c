/*
 * PBL - Photon Bus Loader, core logic (HAL independent).
 * Built for the target with hal_f0.h, for the host simulator with ../sim/hal_sim.h.
 * Size budget: whole PBL (header + entry + core + HAL) <= 1024 B.
 *
 * SPDX-License-Identifier: MPL-2.0
 */
#include <stdint.h>
#include "pbl.h"

#ifdef PBL_SIM
#include "../sim/hal_sim.h"
#else
#include "hal_f0.h"
#endif

/* No initialised data: everything is set explicitly in pbl_start(), so the
 * image needs neither .data copy nor .bss clear.  One struct = one base
 * register on Cortex-M0 (smaller code). */
static struct {
    uint8_t  ver;                       /* ver..last_crc = STATUS reply bytes */
    uint8_t  state;
    uint8_t  err;
    uint8_t  my_addr;
    uint32_t last_crc;
    uint8_t  pkt[PBL_MAX_PACKET];
    uint8_t  page0[PBL_PAGE_SIZE];      /* RAM shadow of flash page 0 */
} g;
#define pkt      g.pkt
#define page0    g.page0
#define err      g.err
#define state    g.state
#define my_addr  g.my_addr
#define last_crc g.last_crc

/* last error wins; cleared by STATUS */
#define SET_ERR(e) (err = (e))

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

/* Photon CRC-8 (poly 0x07, init 0) over the packet, skipping the CRC byte 4. */
static uint32_t crc8_pkt(uint32_t total)
{
    uint32_t crc = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (i == 4)
            continue;
        crc ^= (uint32_t)pkt[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u)
                crc ^= 0x8380u;
            crc <<= 1;
        }
    }
    return (crc >> 8) & 0xFFu;
}

/* zlib CRC-32 of the image as it will be after commit (page 0 from RAM).
 * Software, bitwise: identical code in the target and simulator builds
 * (~0.3 s for 31 KB at 8 MHz, runs after the COMMIT reply). */
static uint32_t crc32_img(uint32_t len)
{
#ifdef PBL_HW_CRC
    hal_crc_begin();
    for (uint32_t i = 0; i < len; i++)
        hal_crc_feed((i < PBL_PAGE_SIZE) ? page0[i] : hal_flash_read8(i));
    return hal_crc_end();
#else
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= (i < PBL_PAGE_SIZE) ? page0[i] : hal_flash_read8(i);
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
#endif
}

/* Program len bytes (even) from src at flash offset off.  With shadow != 0,
 * offsets inside page 0 go to the RAM shadow instead.  Already-equal halfwords
 * are skipped, so a retried WRITE is harmless.  Programming a halfword that is
 * not erased fails in the controller and is caught by the read-back. */
static void prog(uint32_t off, const uint8_t *src, uint32_t len, int shadow)
{
    for (uint32_t i = 0; i < len; i += 2, off += 2) {
        uint32_t v = rd16(src + i);
        if (shadow && off < PBL_PAGE_SIZE) {
            *(uint16_t *)&page0[off] = (uint16_t)v;     /* off even, aligned */
            continue;
        }
        if (hal_flash_read16(off) == v)
            continue;
        hal_flash_prog16(off, v);
        if (hal_flash_read16(off) != v)
            err = PBL_E_VERIFY;
    }
}

/* Receive one packet addressed to us with a valid CRC. */
static void rx_packet(void)
{
    for (;;) {
        uint32_t n = 0, need = 5;
        int c = hal_uart_getc(1);
        for (;;) {
            if (n < sizeof pkt)
                pkt[n] = (uint8_t)c;
            n++;
            if (n == 4)
                need = 5u + pkt[3];
            if (n == need)
                break;
            c = hal_uart_getc(0);
            if (c < 0)
                break;                      /* inter-byte gap: resync */
        }
        if (n == need && need <= sizeof pkt && pkt[0] == my_addr &&
            crc8_pkt(n) == pkt[4])
            return;
    }
}

/* reply: header + status (+ data already in pkt[6..]) */
static void reply(uint32_t rlen)
{
    pkt[0] = 0x00;                          /* to controller */
    pkt[1] = my_addr;
    pkt[3] = (uint8_t)rlen;                 /* pkt[2] = packet id, echoed */
    pkt[5] = err;
    pkt[4] = (uint8_t)crc8_pkt(5u + rlen);
    hal_uart_send(pkt, 5u + rlen);
}

/* Long operations (ERASE, COMMIT) run after their reply: the host waits
 * before the next packet and any error shows up in the next reply. */
PBL_NORETURN void pbl_start(uint32_t addr, uint32_t from_reset)
{
    hal_init();
    state = 0;
    if (from_reset) {
        /* entered through the recovery stub that ERASE(0) left in page 0 */
        addr = hal_flash_read8(12);         /* stub word 3 */
        state = PBL_ST_RECOVERY | PBL_ST_ARMED;
    }
    my_addr = (uint8_t)addr;
    g.ver = PBL_VERSION;
    err = PBL_OK;
    last_crc = 0;
    for (uint32_t i = 0; i < PBL_PAGE_SIZE; i++)
        page0[i] = 0xFF;

    for (;;) {
        rx_packet();
        uint32_t plen = pkt[3];
        uint32_t a = rd16(&pkt[6]);

        switch (pkt[5]) {
        case PBL_CMD_STATUS:
            /* ver, state, err, addr, last_crc (LE) */
            for (int i = 0; i < 8; i++)
                pkt[6 + i] = ((const uint8_t *)&g)[i];
            reply(PBL_STATUS_LEN);
            err = PBL_OK;                   /* STATUS clears the error */
            break;

        case PBL_CMD_ERASE:
            a = pkt[6];
            if (a >= PBL_APP_PAGES) {       /* never the PBL page */
                err = PBL_E_RANGE;
                reply(1);
                break;
            }
            reply(1);
            hal_flash_erase(a);
            if (a == 0) {
                uint32_t stub[4] = { PBL_RAM_TOP, PBL_ENTRY_RESET_ADDR,
                                     PBL_RECOVERY_MAGIC, my_addr };
                prog(0, (const uint8_t *)stub, sizeof stub, 0);
                state |= PBL_ST_ARMED;
            }
            break;

        case PBL_CMD_WRITE:
            /* odd length: the extra byte is caught by the commit CRC */
            if (plen < 5 || (a & 1u) || a + plen - 3u > PBL_APP_MAX)
                err = PBL_E_RANGE;
            else
                prog(a, &pkt[8], plen - 3u, 1);
            reply(1);
            break;

        case PBL_CMD_COMMIT: {
            uint32_t expect = rd16(&pkt[8]) | (rd16(&pkt[10]) << 16);
            reply(1);
            HAL_BUSY_MS(250);               /* simulator only: models CRC time */
            /* len > PBL_APP_MAX only reads the PBL page: CRC mismatch */
            last_crc = crc32_img(a);
            if (last_crc != expect) {
                err = PBL_E_CRC;
                break;
            }
            /* critical window (~65 ms): page 0 erased until reprogrammed */
            hal_flash_erase(0);
            prog(0, page0, PBL_PAGE_SIZE, 0);
            if (err == PBL_OK)
                state = (uint8_t)((state & ~PBL_ST_ARMED) | PBL_ST_COMMITTED);
            break;
        }

        case PBL_CMD_RESET:
            reply(1);
            hal_reset();

        default:
            err = PBL_E_CMD;
            reply(1);
            break;
        }
        HAL_BUSY_DONE();
    }
}
