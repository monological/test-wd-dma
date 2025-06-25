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

/* ------------ vdip / vled ----------------------------------------------- */

static uint8_t led_cmd(uint8_t func, uint8_t sel) {
    uint16_t cmd = (sel << 4) | func;          /* byte=0 on write */
    fpga_mgmt_set_vDIP(SLOT, cmd);
    for(int i = 0; i < 200; i++) {             /* ~2 ms total */
        uint16_t v;
        if(fpga_mgmt_get_vLED_status(SLOT, &v)) break;
        if(((v >> 4) & 0xF) == sel && (v & 0xF) == func)
            return (uint8_t)(v >> 8);
        usleep(10);
    }
    return 0;
}

static void dump_vdip_page(void) {
    uint8_t buf[16];
    for(uint8_t s = 0; s < 16; s++) buf[s] = led_cmd(0x0, s);   /* func 0 */

    puts("vled contents:");
    for(int i = 0; i < 16; i++)
        printf("%02x%s", buf[i], (i % 16 == 15) ? "\n" : " ");
}

/* -------------- counter helpers --------------------------------------- */

struct cntr_desc { uint8_t idx; const char *name; };

static const struct cntr_desc snap_desc[] = {
    { 1,  "pad in"    },
    { 2,  "pad out"   },
    { 3,  "sha out"   },
    { 4,  "sv0 out"   },
    { 5,  "sv2_f    " },
    { 6,  "sv2 out"   },
    { 7,  "ecc out"   },

    {10,  "input count"      },
    {11,  "input fifo fill"  },
    {12,  "input drops"      },
    {13,  "result count"     },
    {14,  "result fifo fill" },
    {15,  "result drops"     },
    {16,  "result dma count" }
};

/* read and print snapshot counters */
static void print_snapshot(wd_wksp_t *wd) {
    puts("\n--- counters ---");
    for(size_t i = 0; i < sizeof(snap_desc)/sizeof(snap_desc[0]); i++) {
        uint32_t val = wd_rd_cntr(wd, SLOT, snap_desc[i].idx);
        printf("  %-18s : %10u (idx %u)\n",
               snap_desc[i].name, val, snap_desc[i].idx);
    }
    printf("\n");
}

/* -------------- main --------------------------------------------------- */

int main(void) {
    wd_wksp_t wd = {0};

    puts("allocating hugepage...");
    void *hp = alloc_hugepage();
    if(wd_init_pci(&wd, SLOT_MASK)) {
        fprintf(stderr, "wd_init_pci failed\n");
        return 1;
    }

    puts("initializing verify request...");
    wd_ed25519_verify_init_req(&wd, 1, DEPTH, hp);

    dump_vdip_page();

    /* captured AW-address (func 0xE) */
    uint64_t awaddr = 0;
    for(uint8_t s = 0; s < 8; s++) awaddr |= ((uint64_t)led_cmd(0xE, s)) << (s*8);
    printf("captured AW-address  : 0x%016" PRIx64 "\n", awaddr);

    /* BRESP + PCIM handshake bitmap (func 0xD, sel 0 / 1) */
    uint8_t bresp = led_cmd(0xD, 0);
    uint8_t proto = led_cmd(0xD, 1);
    printf("last BRESP           : 0x%x (bits[1:0])\n", bresp & 0x3);
    printf("PCIM handshake bits  : 0x%02x (ar/r/aw/w {v,r})\n", proto);

    /* dummy verify request */
    uint8_t msg[64] = {0}, sig[64] = {0}, pub[32] = {0};
    uint64_t m_seq = 1;

    puts("sending verify request...");
    wd_ed25519_verify_req(&wd, msg, sizeof(msg),
                          sig, pub, m_seq, 0, 0x3, sizeof(msg));

    /* snapshot counters */
    wd_snp_cntrs(&wd, SLOT);

    print_snapshot(&wd);

    /* wait for DMA to complete */
    struct timespec ts = {0, 5 * 1000 * 1000};
    nanosleep(&ts, NULL);

    uint8_t *line = (uint8_t *)hp + ((m_seq & (DEPTH - 1)) << 5);
    _mm_clflush(line); _mm_mfence();

    /* should be non-zero if DMA was successful */
    puts("\nmcache line after DMA:");
    hexdump32(line);

    uint16_t vled;
    if(!fpga_mgmt_get_vLED_status(SLOT, &vled))
        printf("vled raw 0x%04x  (func=%x sel=%u byte=0x%02x)\n",
               vled, vled & 0xF, (vled >> 4) & 0xF, (vled >> 8) & 0xFF);

    wd_free_pci(&wd);
    munmap(hp, HP_SIZE);
    return 0;
}
