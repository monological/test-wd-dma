/* test_dma.c – minimal loop-back check for Wiredancer ED25519 path */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <immintrin.h>

#include <fpga_mgmt.h>
#include "wd_f1.h"

#define HP_SIZE   (2UL << 20)
#define DEPTH     1024
#define SLOT      0
#define SLOT_MASK (1UL << SLOT)

/* -------------- helpers ------------------------------------------------ */

static void *alloc_hugepage(void) {
    void *p = mmap(NULL, HP_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
                   -1, 0);
    if(p == MAP_FAILED) { perror("mmap hugepage"); exit(1); }
    memset(p, 0, HP_SIZE);
    return p;
}

static void hexdump32(const void *buf) {
    const uint8_t *b = buf;
    for(int i = 0; i < 32; i++)
        printf("%02x%s", b[i], (i % 16 == 15) ? "\n" : " ");
}

static uint8_t read_vdip_byte(uint32_t slot, uint8_t sel) {
    uint16_t cmd = (sel << 4) | 0x0;             /* func = 0 (read) */
    fpga_mgmt_set_vdip(slot, cmd);

    for(int i = 0; i < 100; i++) {               /* ~1 ms timeout   */
        uint16_t vled;
        if(fpga_mgmt_get_vLED_status(slot, &vled)) break;

        if(((vled >> 4) & 0xF) == sel && (vled & 0xF) == 0x0)
            return (uint8_t)(vled >> 8);

        usleep(10);
    }
    return 0;
}

static void dump_vdip_via_vled(uint32_t slot) {
    uint8_t buf[16];
    for(uint8_t sel = 0; sel < 16; sel++)
        buf[sel] = read_vdip_byte(slot, sel);

    puts("vdip content (16 bytes) read via vLED:");
    for(int i = 0; i < 16; i++)
        printf("%02x%s", buf[i], (i % 16 == 15) ? "\n" : " ");
}

/* -------------- main --------------------------------------------------- */

int main(void) {
    wd_wksp_t wd = {0};

    void *hp = alloc_hugepage();
    if(wd_init_pci(&wd, SLOT_MASK)) {
        fprintf(stderr, "wd_init_pci failed\n");
        return 1;
    }

    wd_ed25519_verify_init_req(&wd, 1, DEPTH, hp);

    dump_vdip_via_vled(SLOT);

    /* dummy verify request */
    uint8_t msg[64] = {0}, sig[64] = {0}, pub[32] = {0};
    uint64_t m_seq = 1;

    puts("sending verify request...");
    wd_ed25519_verify_req(&wd, msg, sizeof(msg),
                          sig, pub, m_seq, 0, 0x3, sizeof(msg));

    wd_snp_cntrs(&wd, SLOT);
    printf("rx_pkts=%u  tx_dma=%u\n",
           wd_rd_cntr(&wd, SLOT, 0), wd_rd_cntr(&wd, SLOT, 1));

    struct timespec ts = {0, 5 * 1000 * 1000};
    nanosleep(&ts, NULL);

    uint8_t *line = (uint8_t *)hp + ((m_seq & (DEPTH - 1)) << 5);
    _mm_clflush(line); _mm_mfence();

    /* should be non-zero if DMA was successful */
    puts("mcache line after DMA:");
    hexdump32(line);

    uint16_t vled;
    if(!fpga_mgmt_get_vLED_status(SLOT, &vled))
        printf("vLED raw 0x%04x  (func=%x sel=%u byte=0x%02x)\n",
               vled, vled & 0xF, (vled >> 4) & 0xF, (vled >> 8) & 0xFF);

    wd_free_pci(&wd);
    munmap(hp, HP_SIZE);
    return 0;
}
