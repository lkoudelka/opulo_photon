/*
 * PBL - Photon Bus Loader, protocol + memory contract (v1)
 *
 * A 1 KB bootloader living in the LAST flash page of the Photon feeder
 * (STM32F031x6, 32 x 1 KB pages).  It is entered explicitly from the
 * application (VENDOR_OPTIONS 0xBF / sub 0x21 + magic) and speaks the normal
 * Photon framing, so it can be driven through Marlin's `M485` G-code.
 *
 * Memory contract
 *   0x08000000 .. 0x08007BFF  application (standard release .bin, unmodified
 *                             link address), max 31 744 B (31 pages)
 *   0x08007C00 .. 0x08007FFF  PBL (this code), header at 0x08007C00
 *
 * The application image is flashed byte-for-byte as released; the only
 * requirement on future releases is size <= PBL_APP_MAX.
 *
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef PBL_H
#define PBL_H

#define PBL_VERSION          1u

#define PBL_FLASH_BASE       0x08000000u
#define PBL_PAGE_SIZE        1024u
#define PBL_APP_PAGES        31u
#define PBL_APP_MAX          (PBL_APP_PAGES * PBL_PAGE_SIZE)          /* 31744 */
#define PBL_BASE             (PBL_FLASH_BASE + PBL_APP_MAX)           /* 0x08007C00 */
#define PBL_RAM_TOP          0x20001000u   /* stay inside the 4 KB datasheet RAM */

/* Header at PBL_BASE, read by the application before jumping. */
#define PBL_HEADER_MAGIC     0x314C4250u   /* "PBL1" little endian */
/* struct pbl_header { u32 magic; u16 version; u16 reserved;
 *                     u32 entry_app (thumb, arg r0 = bus address);
 *                     u32 entry_reset (thumb); }                       */

/* While an update is in progress page 0 holds a recovery stub instead of
 * the application vectors, so a reset / power loss lands in PBL again:
 *   word0 = PBL_RAM_TOP  word1 = entry_reset  word2 = PBL_RECOVERY_MAGIC
 *   word3 = bus address that was in use                                 */
#define PBL_RECOVERY_MAGIC   0x52424C50u   /* "PLBR" */

/* Application side (VENDOR_OPTIONS 0xBF) sub-commands, options[0]:       */
#define PBL_VENDOR_INFO      0x20  /* reply options: 'P','B','L',app_proto,bl_version(0=none) */
#define PBL_VENDOR_ENTER     0x21  /* options[1..4] = 'P','B','L','!' ; reply as INFO, then jump */
#define PBL_ENTER_MAGIC      "PBL!"

/* Bootloader commands (Photon framing, unicast to the feeder address).
 * Every reply: header + status(1) [+ data].  status = error code of the
 * last failed operation since the last STATUS (0 = OK); long operations
 * (ERASE, COMMIT) run after their reply, so their result shows up in the
 * NEXT reply.                                                            */
#define PBL_CMD_STATUS       0xB0  /* -> ver, state, err, addr, last_crc32(LE); clears err.
                                      (no UID: identity is checked with the app's
                                      GET_FEEDER_ID before entering PBL)       */
#define PBL_STATUS_LEN       9     /* status byte + 8 data bytes */
#define PBL_CMD_ERASE        0xB1  /* page(1)       ; reply first, then erase (<=40 ms)     */
#define PBL_CMD_WRITE        0xB2  /* off(LE16), data[2..24], even ; page 0 goes to RAM     */
#define PBL_CMD_COMMIT       0xB3  /* len(LE16), crc32(LE32) ; reply first, then verify and
                                      program page 0 (~0.3 s)                              */
#define PBL_CMD_RESET        0xB4  /* reply, then system reset                              */

/* state bits */
#define PBL_ST_ARMED         0x01  /* page 0 holds the recovery stub   */
#define PBL_ST_COMMITTED     0x02  /* new image verified + page 0 written */
#define PBL_ST_RECOVERY      0x04  /* entered from reset (stub), not from app */

/* status / error codes (0x00, 0x05 shared with Photon) */
#define PBL_OK               0x00
#define PBL_E_FLASH          0x05  /* flash controller reported an error */
#define PBL_E_RANGE          0x10
#define PBL_E_ALIGN          0x11
#define PBL_E_NOT_ERASED     0x12
#define PBL_E_VERIFY         0x13
#define PBL_E_CRC            0x14
#define PBL_E_CMD            0x15

#define PBL_MAX_PACKET       32    /* = Marlin RS485_SEND_BUFFER_SIZE */
#define PBL_WRITE_MAX        (PBL_MAX_PACKET - 8)                     /* 24 */

#endif
