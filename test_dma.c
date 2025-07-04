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

static void
dump_nonzero_lines(const void *base, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)base;

    for (size_t off = 0; off < bytes; off += 32) {
        /* load the 32-byte block and test if it is all zero           */
        __m256i v = _mm256_load_si256((const __m256i *)(p + off));
        if (_mm256_testz_si256(v, v))              /* all-zero → skip  */
            continue;

        /* at least one non-zero byte – print the whole cache line     */
        printf("%06zx :", off);
        for (int i = 0; i < 32; i++)
            printf(" %02x", p[off + i]);
        putchar('\n');
    }
}

static inline void clflush_hugepage(void *base, size_t len)
{
    const char *p = base;
    for (size_t off = 0; off < len; off += 64)
        _mm_clflush(p + off);
    _mm_mfence();                /* finish all flushes */
}

/* ------------ vdip / vled ----------------------------------------------- */

static uint8_t get_vled_byte(uint8_t func, uint8_t sel) {
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

static void dump_vled(void) {
    uint8_t buf[16];
    for(uint8_t s = 0; s < 16; s++) buf[s] = get_vled_byte(0x0, s);   /* func 0 */

    puts("vled contents:");
    for(int i = 0; i < 16; i++)
        printf("%02x%s", buf[i], (i % 16 == 15) ? "\n" : " ");
}

/* read a 32-bit counter over vDIP/vLED (func 0xC edge or 0xB handshake) */
static uint32_t read_vled_counter(uint8_t func, uint8_t ctr_sel) {
    uint32_t v = 0;
    for(uint8_t byte = 0; byte < 4; byte++) {
        uint8_t sel = (ctr_sel << 2) | byte;      /* sel_idx[3:2] | sel_idx[1:0] */
        v |= ((uint32_t)get_vled_byte(func, sel)) << (byte * 8);
    }
    return v;
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

    /* zero the whole 2 MiB page and push clean data to DRAM */
    memset(hp, 0, HP_SIZE);
    clflush_hugepage(hp, HP_SIZE);

    if (wd_init_pci(&wd, SLOT_MASK)) {
        fprintf(stderr, "wd_init_pci failed\n");
        return 1;
    }

    printf("allocated hugepage address  : 0x%016" PRIx64 "\n", hp);

    puts("initializing verify request...");
    wd_ed25519_verify_init_req(&wd, 1, DEPTH, hp);

    struct timespec ts = {0, 5 * 1000 * 2000};   /* 10 ms */
    nanosleep(&ts, NULL);                         /* wait for init */

    /* dummy verify request */
    uint8_t msg[64] = {0}, sig[64] = {0}, pub[32] = {0};
    uint64_t m_seq = 1;

    puts("sending verify request...");
    wd_ed25519_verify_req(&wd, msg, sizeof(msg),
                          sig, pub, m_seq, 0, 0x3, sizeof(msg));

    nanosleep(&ts, NULL);                         /* allow CL to issue AW */

    dump_vled();

    /* poll for non-zero AW address (func 0xE) */
    uint64_t awaddr = 0;
    puts("waiting for pcim awaddr...");
    for (int i = 0; i < 200 && awaddr == 0; i++) {
        usleep(50);
        awaddr = 0;
        for (uint8_t s = 0; s < 8; s++)
            awaddr |= (uint64_t)get_vled_byte(0xE, s) << (s * 8);
    }
    printf("captured pcim awaddr from vled: 0x%016" PRIx64 "\n", awaddr);

    /* BRESP + PCIM handshake bitmap (func 0xD, sel 0 / 1) */
    uint8_t bresp = get_vled_byte(0xD, 0);
    uint8_t proto = get_vled_byte(0xD, 1);
    printf("last BRESP           : 0x%x (bits[1:0])\n", bresp & 0x3);
    printf("PCIM handshake bits  : 0x%02x (ar/r/aw/w {v,r})\n", proto);

    /* edge and handshake counters */
    uint32_t edge_awv = read_vled_counter(0xC, 0);
    uint32_t edge_awr = read_vled_counter(0xC, 1);
    uint32_t edge_wv  = read_vled_counter(0xC, 2);
    uint32_t edge_wr  = read_vled_counter(0xC, 3);
    uint32_t hs_aw    = read_vled_counter(0xB, 0);
    uint32_t hs_w     = read_vled_counter(0xB, 1);

    puts("\n--- PCIM counters ---");
    printf("  awvalid edges      : %10u\n", edge_awv);
    printf("  awready edges      : %10u\n", edge_awr);
    printf("  wvalid edges       : %10u\n", edge_wv);
    printf("  wready edges       : %10u\n", edge_wr);
    printf("  AW handshakes      : %10u\n", hs_aw);
    printf("  W  handshakes      : %10u\n\n", hs_w);

    /* captured WSTRB mask (func 0xA) */
    uint64_t wstrb = 0;
    for(uint8_t s = 0; s < 8; s++) 
        wstrb |= ((uint64_t)get_vled_byte(0xA, s)) << (s*8);
    printf("captured WSTRB mask  : 0x%016" PRIx64 "\n", wstrb);

    wd_snp_cntrs(&wd, SLOT);

    print_snapshot(&wd);

    /* give the DMA engine time to finish, then invalidate CPU cache */
    nanosleep(&ts, NULL);
    clflush_hugepage(hp, HP_SIZE);

    /* should be non-zero if DMA was successful */
    puts("\ndumping non-zero lines in hugepage:");
    dump_nonzero_lines(hp, HP_SIZE);

    wd_free_pci(&wd);
    munmap(hp, HP_SIZE);
    return 0;
}
