/*
 * PBL target HAL for STM32F031x6 (Photon feeder mobo), bare registers.
 * Clock: HSI 8 MHz (reset clock), USART1 57600 8N1 on PA9/PA10,
 * RS-485 ~RE = PA11, DE = PA12 driven as GPIO (same as the application).
 *
 * Entry contract (pbl.h): the application calls entry_app with IRQs
 * disabled after HAL_RCC_DeInit() + HAL_DeInit() + SysTick off, i.e. the
 * same clean-up its existing ROM-bootloader jump already does.  The
 * recovery path starts from a real reset.  So no clock / peripheral
 * de-initialisation is needed here.
 *
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef PBL_HAL_F0_H
#define PBL_HAL_F0_H
#include <stdint.h>

#define PBL_NORETURN __attribute__((noreturn))

typedef volatile uint32_t reg_t;
#define RCC     ((reg_t *)0x40021000u)   /* [5]=AHBENR [6]=APB2ENR */
#define GPIOA   ((reg_t *)0x48000000u)   /* [0]=MODER [3]=PUPDR [5]=ODR [6]=BSRR [9]=AFRH */
#define USART1  ((reg_t *)0x40013800u)   /* [0]=CR1 [2]=CR3 [3]=BRR [7]=ISR [9]=RDR [10]=TDR */
#define FLASH   ((reg_t *)0x40022000u)   /* [1]=KEYR [3]=SR [4]=CR [5]=AR */
#define SCB_AIRCR (*(reg_t *)0xE000ED0Cu)

#define ISR_RXNE      (1u << 5)
#define ISR_TC        (1u << 6)
#define ISR_TXE       (1u << 7)
#define FCR_PG        (1u << 0)
#define FCR_PER       (1u << 1)
#define FCR_STRT      (1u << 6)

/* GPIOA: PA2 out low (MOTOR_ENABLE off), PA4/PA7 out low = LED B+R on
 * (active low -> magenta = "in bootloader"), PA6 out high = LED G off,
 * PA9/PA10 AF1 USART1, PA11 out low (~RE: receiver on), PA12 out low (DE off),
 * PA13/PA14 SWD (reset value).  Every other pin: input, as after reset. */
#define PA_MODER      0x29685110u
#define PA_ODR        (1u << 6)
#define PA_PUPDR      (0x24000000u | (1u << 20))   /* SWD defaults + PA10 pull-up */
#define PA_AFRH       0x00000110u

#define RX_GAP_LOOPS  4000u         /* ~3-4 ms at 8 MHz */
#define TX_TURN_LOOPS 100u          /* ~100 us bus turnaround */

void pbl_entry_reset(void);
#define PBL_ENTRY_RESET_ADDR ((uint32_t)&pbl_entry_reset)
#define HAL_BUSY_MS(ms) ((void)0)
#define HAL_BUSY_DONE() ((void)0)

/* register init as (address, value) table: smaller than inline stores */
static const uint32_t hal_init_tab[] = {
    0x40021014u, 0x00020014u,               /* RCC_AHBENR  = SRAM|FLITF|GPIOA */
    0x40021018u, 0x00004000u,               /* RCC_APB2ENR = USART1 */
    0x48000014u, PA_ODR,
    0x48000024u, PA_AFRH,
    0x4800000Cu, PA_PUPDR,
    0x48000000u, PA_MODER,
    0x4001380Cu, 139u,                      /* USART1_BRR: 8 MHz / 57600 */
    0x40013808u, 1u << 12,                  /* USART1_CR3: OVRDIS */
    0x40013800u, 0x0Du,                     /* USART1_CR1: UE | RE | TE */
    0x40022004u, 0x45670123u,               /* FLASH_KEYR: unlock (until reset) */
    0x40022004u, 0xCDEF89ABu,
};

static inline void hal_init(void)
{
    const uint32_t *t = hal_init_tab;
    __asm volatile ("" : "+r"(t));          /* keep it a loop, not 22 stores */
    for (uint32_t i = 0; i < sizeof hal_init_tab / 4; i += 2)
        *(reg_t *)t[i] = t[i + 1];
}

static inline int hal_uart_getc(int block)
{
    uint32_t n = RX_GAP_LOOPS;
    while (!(USART1[7] & ISR_RXNE))
        if (!block && --n == 0)
            return -1;
    return (int)(USART1[9] & 0xFFu);
}

static inline void hal_uart_send(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = TX_TURN_LOOPS; i; i--)
        __asm volatile ("");                /* ~100 us bus turnaround */
    GPIOA[6] = (1u << 11) | (1u << 12);     /* ~RE high, DE high */
    while (n--) {
        while (!(USART1[7] & ISR_TXE))
            ;
        USART1[10] = *p++;
    }
    while (!(USART1[7] & ISR_TC))
        ;
    GPIOA[6] = (1u << 27) | (1u << 28);     /* DE low, ~RE low */
}

static inline uint32_t hal_flash_read8(uint32_t off)
{
    return *(volatile const uint8_t *)(0x08000000u + off);
}

static inline uint32_t hal_flash_read16(uint32_t off)
{
    return *(volatile const uint16_t *)(0x08000000u + off);
}

/* Result is checked by read-back (erase: by the following programming). */
static void hal_flash_wait(void)
{
    while (FLASH[3] & 1u)
        ;
    FLASH[3] = 0x34u;                       /* clear EOP | WRPRTERR | PGERR */
    FLASH[4] = 0;
}

static void hal_flash_erase(uint32_t page)
{
    FLASH[4] = FCR_PER;
    FLASH[5] = 0x08000000u + page * 1024u;
    FLASH[4] = FCR_PER | FCR_STRT;
    hal_flash_wait();
}

static void hal_flash_prog16(uint32_t off, uint32_t v)
{
    FLASH[4] = FCR_PG;
    *(volatile uint16_t *)(0x08000000u + off) = (uint16_t)v;
    hal_flash_wait();
}

#ifdef PBL_HW_CRC
/* Optional: CRC peripheral as zlib CRC-32 (poly 0x04C11DB7 fixed, init
 * 0xFFFFFFFF, REV_IN = byte bit-reversal, REV_OUT, final XOR in software),
 * fed with 8-bit writes to DR.  Saves only ~8 B and its semantics must be
 * verified on silicon first, so the default build uses software CRC.
 * (Also needs CRCEN, bit 6, in RCC_AHBENR above.) */
#define CRCU ((reg_t *)0x40023000u)
static inline void hal_crc_begin(void) { CRCU[2] = (1u << 7) | (1u << 5) | 1u; }
static inline void hal_crc_feed(uint32_t b) { *(volatile uint8_t *)&CRCU[0] = (uint8_t)b; }
static inline uint32_t hal_crc_end(void) { return ~CRCU[0]; }
#endif

static inline PBL_NORETURN void hal_reset(void)
{
    __asm volatile ("dsb");
    SCB_AIRCR = 0x05FA0004u;
    for (;;)
        ;
}

#endif
