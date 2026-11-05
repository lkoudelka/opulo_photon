/*
 * Host build of the PBL core:  pbl_sim <flash.bin> <bus-address> <app|reset>
 *   app   = entered from the application (VENDOR 0x21)
 *   reset = entered through the recovery stub in page 0
 * SPDX-License-Identifier: MPL-2.0
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PBL_SIM 1
#include "../bootloader/pbl_core.c"

uint8_t *sim_flash;

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s flash.bin address app|reset\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0 || lseek(fd, 0, SEEK_END) != 32768) {
        fprintf(stderr, "flash file must exist and be 32768 bytes\n");
        return 2;
    }
    sim_flash = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sim_flash == MAP_FAILED)
        return 2;
    pbl_start((uint32_t)strtoul(argv[2], NULL, 0), strcmp(argv[3], "reset") == 0);
}
