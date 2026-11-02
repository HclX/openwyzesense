// Native tests of nvstore.c and the gateway's use of it, on a RAM page that behaves like flash (programming
// can only clear bits), and on the stock dongles' pages (ground_truth/firmware/gateway_*/FLASH.bin).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gateway.h"
#include "host_proto.h"
#include "nvstore.h"

static int failures, checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

static uint8_t flash[NV_PAGE_SIZE];
static int erases;
static long budget = -1;  // flash operations left before a simulated power cut (-1: no cut); a byte programmed or an
                          // erase each take one; an erase that is cut leaves the page half erased
static long used;         // operations done since the last reset of the count

static bool program(void *ctx, uint32_t off, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (off + len > NV_PAGE_SIZE) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (budget == 0) {
            return false;  // power gone
        }
        budget -= budget > 0;
        used++;
        CHECK((flash[off + i] & data[i]) == data[i]);  // NOR flash: bits only go from 1 to 0
        flash[off + i] &= data[i];
    }
    return true;
}

static bool erase(void *ctx)
{
    (void)ctx;
    if (budget == 0) {
        return false;
    }
    used++;
    if (budget == 1) {  // cut during the erase: half of it done
        memset(flash, 0xFF, sizeof flash / 2);
        budget = 0;
        return false;
    }
    budget -= budget > 0;
    memset(flash, 0xFF, sizeof flash);
    erases++;
    return true;
}

static nvstore_t store(void)
{
    nvstore_t nv = {flash, program, erase, NULL, 0};
    CHECK(nv_open(&nv));
    return nv;
}

static void nop_tx(void *c, const uint8_t *f, size_t n, uint32_t g)
{
    (void)c, (void)f, (void)n, (void)g;
}
static void nop_host(void *c, const uint8_t *p, size_t n)
{
    (void)c, (void)p, (void)n;
}
static const gw_io_t io = {nop_tx, nop_host, NULL, NULL, NULL};

static void load_page(const char *path)  // a 4 KB page, or page 30 of a 128 KB flash dump
{
    FILE *f = fopen(path, "rb");
    long size = f && !fseek(f, 0, SEEK_END) ? ftell(f) : -1;
    if (!f || fseek(f, size == NV_PAGE_SIZE ? 0 : 0x1E000, SEEK_SET) ||
        fread(flash, 1, NV_PAGE_SIZE, f) != NV_PAGE_SIZE) {
        perror(path);
        exit(2);
    }
    fclose(f);
}

// A stock page (the v1.4 / v1.8 dongles') is not ours: formatted on opening, its data gone (the flashing tools carry
// it over: tools/nv_json.py, #61); the gateway then starts with an empty table and keeps what it pairs
static void test_stock_pages(const char *v14, const char *v18)
{
    const char *pages[] = {v14, v18};
    for (int k = 0; k < 2; k++) {
        gateway_t gw;
        load_page(pages[k]);
        nvstore_t nv = store();
        CHECK(!memcmp(flash, "OWNV\x01\xff\xff\xff", 8));
        gw_init(&gw, 0x65FE, 0xD425, 1, (const uint8_t *)"00000000", &io);
        CHECK(!gw_load(&gw, &nv) && gw.n_sensors == 0);
        CHECK(gw_save_network(&gw) && gw_add_sensor(&gw, (const uint8_t *)"77FED830", 0x0001));
        gateway_t again;
        nvstore_t nv2 = store();
        gw_init(&again, 0, 0, 0, (const uint8_t *)"00000000", &io);
        CHECK(gw_load(&again, &nv2) && again.pan == 0x65FE && again.n_sensors == 1);
        CHECK(!memcmp(again.sensors[0].mac, "77FED830", 8) && again.sensors[0].short_addr == 1);
    }
}

static void test_write_and_compact(void)
{
    uint32_t want[8][4] = {{0}};
    bool have[8] = {false};
    unsigned seed = 1;
    erase(NULL);
    erases = 0;
    nvstore_t nv = store();
    for (int round = 0; round < 1000; round++) {
        seed = seed * 1103515245 + 12345;
        uint16_t sub = (seed >> 8) % 8;
        if ((seed >> 20) % 7 == 0) {
            CHECK(nv_delete(&nv, 9, sub));
            have[sub] = false;
        } else {
            uint32_t v[4] = {seed, (uint32_t)round, sub, ~seed};
            CHECK(nv_write(&nv, 9, sub, (const uint8_t *)v, sizeof v));
            memcpy(want[sub], v, sizeof v);
            have[sub] = true;
        }
    }
    CHECK(erases >= 4);  // ~170 records a page: compacted every ~165 writes
    nvstore_t reopened = {flash, program, erase, NULL, 0};
    CHECK(nv_open(&reopened));
    for (uint16_t sub = 0; sub < 8; sub++) {
        uint16_t len;
        const uint8_t *d = nv_find(&reopened, 9, sub, &len);
        CHECK(have[sub] ? d && len == 16 && !memcmp(d, want[sub], 16) : d == NULL);
    }
}

static void test_gateway_roundtrip(void)
{
    gateway_t gw, back;
    erase(NULL);
    nvstore_t nv = store();
    gw_init(&gw, 0x65FE, 0x0F0F, 0x00124B000E07D830ull, (const uint8_t *)"7707D830", &io);
    CHECK(!gw_load(&gw, &nv));  // blank: no network yet
    gw.pan = 0x65FE, gw.short_addr = 0x0F0F, gw.ext = 0x00124B000E07D830ull;
    CHECK(gw_save_network(&gw));
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"77FFFF01", 1));
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"77FFFF02", 2));
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"77FFFF03", 3));
    uint8_t p[64];
    gw_host(&gw, p, host_build(p, HOST_DELETE_SENSOR, (const uint8_t *)"77FFFF02", 8, false));
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"77FFFF04", 4));  // takes the free sub-ID 1

    nvstore_t nv2 = {flash, program, erase, NULL, 0};
    CHECK(nv_open(&nv2));
    gw_init(&back, 0, 0, 0, (const uint8_t *)"7707D830", &io);
    CHECK(gw_load(&back, &nv2) && back.pan == 0x65FE && back.short_addr == 0x0F0F && back.ext == gw.ext);
    CHECK(back.n_sensors == 3);
    CHECK(!memcmp(back.sensors[0].mac, "77FFFF01", 8) && !memcmp(back.sensors[1].mac, "77FFFF04", 8) &&
          !memcmp(back.sensors[2].mac, "77FFFF03", 8));
    CHECK(back.sensors[1].short_addr == 4 && back.sensors[1].nv_sub == 1);
}

// After a power cut (or with any page at all): the next start opens the page, reads it without going out of bounds
// (ASan), and can store a new record that a later start finds. Earlier records may be lost (the user's rule, #38).
static int lost;

static void boots(const char *what, long k)
{
    budget = -1;
    nvstore_t nv = {flash, program, erase, NULL, 0};
    bool ok = nv_open(&nv);
    CHECK(ok);
    if (!ok) {
        fprintf(stderr, "  %s, cut after %ld\n", what, k);
        return;
    }
    for (uint16_t item = 0; item < 12; item++) {
        for (uint16_t sub = 0; sub < 10; sub++) {
            uint16_t len = 0;
            const uint8_t *d = nv_find(&nv, item, sub, &len);
            CHECK(!d || (d >= flash && d + len <= flash + NV_PAGE_SIZE));
        }
    }
    uint32_t v[2] = {0x12345678, (uint32_t)k};
    bool wrote = nv_write(&nv, 77, 1, (const uint8_t *)v, sizeof v);
    CHECK(wrote);
    nvstore_t again = {flash, program, erase, NULL, 0};
    uint16_t len = 0;
    const uint8_t *d = nv_open(&again) ? nv_find(&again, 77, 1, &len) : NULL;
    CHECK(d && len == sizeof v && !memcmp(d, v, sizeof v));
    if (!wrote || !d) {
        fprintf(stderr, "  %s, cut after %ld: the page no longer takes a record (write %d, end %u, found %d)\n", what,
                k, wrote, nv.end, d != NULL);
    }
}

static void fill(int records, int pad)  // a page with records items 1..records, then pad filler writes of item 9
{
    budget = -1;
    erase(NULL);
    nvstore_t nv = store();
    for (int i = 1; i <= records; i++) {
        uint32_t v[3] = {(uint32_t)i, 0xA5A5A5A5u, (uint32_t)~i};
        CHECK(nv_write(&nv, (uint16_t)i, 0, (const uint8_t *)v, sizeof v));
    }
    for (int i = 0; pad && nv.end + 24 <= NV_PAGE_SIZE; i++) {  // pad: up to the last record that fits
        uint32_t v[4] = {(uint32_t)i, 1, 2, 3};
        CHECK(nv_write(&nv, 9, (uint16_t)(i % 4), (const uint8_t *)v, sizeof v));
    }
}

static void op(int which)
{
    nvstore_t nv = {flash, program, erase, NULL, 0};
    uint32_t v[4] = {0xDEADBEEF, 7, 8, 9};  // 24 bytes with its header: does not fit a padded page
    if (!nv_open(&nv)) {
        return;
    }
    if (which == 2) {
        nv_delete(&nv, 2, 0);
    } else {
        nv_write(&nv, 3, 0, (const uint8_t *)v, sizeof v);
    }
}

static void test_power_cuts(void)
{
    static uint8_t start[NV_PAGE_SIZE];
    static const struct {
        const char *what;
        int records, pad, op;
    } cases[] = {
        {"replacing a record", 5, 0, 0},
        {"a write that compacts the page", 5, 1, 0},
        {"deleting a record", 5, 0, 2},
        {"the first write to a blank page", 0, 0, 0},
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        fill(cases[c].records, cases[c].pad);
        if (cases[c].records == 0) {
            memset(flash, 0xFF, sizeof flash);  // blank, not even formatted
        }
        memcpy(start, flash, sizeof start);
        budget = -1, used = 0;
        op(cases[c].op);
        long total = used;
        for (long k = 0; k <= total; k++) {
            memcpy(flash, start, sizeof flash);
            budget = k;
            op(cases[c].op);
            int before = failures;
            boots(cases[c].what, k);
            if (failures != before) {
                break;  // one report per case is enough
            }
            nvstore_t nv = {flash, program, erase, NULL, 0};
            budget = -1;
            if (cases[c].records && nv_open(&nv) && !nv_find(&nv, 1, 0, NULL)) {
                lost++;  // item 1 (untouched by the operation) gone: data lost, still boots
            }
        }
        printf("power cuts, %s: %ld cut points\n", cases[c].what, total + 1);
    }
    printf("power cuts: an untouched record lost at %d cut points (allowed)\n", lost);
}

// Any page at all: random bytes, a valid page with a random tail, random bytes with record headers sprinkled
static void test_any_page_boots(void)
{
    unsigned seed = 7;
    for (int round = 0; round < 3000; round++) {
        int kind = round % 3;
        if (kind == 1) {
            fill(4, 0);
        }
        size_t tail = 8 * (size_t)(round % 256);  // kind 1: this many garbage bytes after the valid records
        for (size_t i = 0; i < sizeof flash; i++) {
            seed = seed * 1103515245 + 12345;
            uint8_t r = (uint8_t)(seed >> 16);
            if (kind == 0 || (kind == 1 && i >= 200 && i < 200 + tail)) {
                flash[i] = r;
            } else if (kind == 2) {
                flash[i] = (r & 7) == 0 ? 0xA5 : (r & 7) == 1 ? 0x00 : (r & 7) < 4 ? 0xFF : r;
            }
        }
        if (kind == 2) {
            memcpy(flash, "OWNV\x01\xff\xff\xff", 8);
        }
        int before = failures;
        boots("a random page", round);
        if (kind == 1) {
            nvstore_t check = {flash, program, erase, NULL, 0};
            CHECK(nv_open(&check) && nv_find(&check, 1, 0, NULL) != NULL);
        }
        if (failures != before) {
            break;
        }
    }
}

static void test_large_compaction_and_guards(void)
{
    budget = -1;
    erase(NULL);
    erases = 0;
    nvstore_t nv = store();

    // 1. > 2560 B of live records (55 * 72 = 3960 B) must survive compaction intact without truncation.
    uint8_t expect[57][60];
    for (uint16_t i = 1; i <= 55; i++) {
        memset(expect[i], (uint8_t)(0x20 + i), sizeof expect[i]);
        expect[i][0] = (uint8_t)i;
        CHECK(nv_write(&nv, 10, i, expect[i], sizeof expect[i]));
    }
    int erases_before = erases;
    memset(expect[1], 0xEE, sizeof expect[1]);
    CHECK(nv_write(&nv, 10, 1, expect[1], sizeof expect[1]));  // 3968 + 72 = 4040 B
    memset(expect[2], 0xDD, sizeof expect[2]);
    CHECK(nv_write(&nv, 10, 2, expect[2], sizeof expect[2]));  // triggers compact() with 3960 B live
    CHECK(erases == erases_before + 1);
    for (uint16_t i = 1; i <= 55; i++) {
        uint16_t len = 0;
        const uint8_t *d = nv_find(&nv, 10, i, &len);
        CHECK(d && len == sizeof expect[i] && !memcmp(d, expect[i], sizeof expect[i]));
    }

    // 2. Full page (8 + 56*72 + 48 = 4088 B): adding a new record that overflows 4096 B must fail BEFORE erasing.
    memset(expect[56], 0xCC, sizeof expect[56]);
    CHECK(nv_write(&nv, 10, 56, expect[56], sizeof expect[56]));  // 4040 B
    uint8_t tail36[36];
    memset(tail36, 0xBB, sizeof tail36);
    CHECK(nv_write(&nv, 11, 0, tail36, sizeof tail36));  // 4088 B
    erases_before = erases;
    uint32_t extra[4] = {1, 2, 3, 4};
    CHECK(!nv_write(&nv, 99, 0, (const uint8_t *)extra, sizeof extra));
    CHECK(erases == erases_before);  // page was NOT erased!
    for (uint16_t i = 1; i <= 56; i++) {
        uint16_t len = 0;
        const uint8_t *d = nv_find(&nv, 10, i, &len);
        CHECK(d && len == sizeof expect[i] && !memcmp(d, expect[i], sizeof expect[i]));
    }
    // Updating an existing 60 B record at 4088 B still fits after skipping its old copy.
    memset(expect[3], 0xAA, sizeof expect[3]);
    CHECK(nv_write(&nv, 10, 3, expect[3], sizeof expect[3]));
    CHECK(erases == erases_before + 1);

    // 3. Transient mid-append write failure marks tail dirty so the next in-memory nv_write compacts instead of
    // overwriting partially programmed bytes (0->1 violation checked in program()).
    erase(NULL);
    nv = store();
    uint32_t base[2] = {0x11223344u, 0x55667788u};
    CHECK(nv_write(&nv, 1, 0, (const uint8_t *)base, sizeof base));
    budget = 6;  // fails halfway through the 12-byte record header
    uint32_t cut[2] = {0x99AABBCCu, 0xDDEEFF00u};
    CHECK(!nv_write(&nv, 2, 0, (const uint8_t *)cut, sizeof cut));
    CHECK(nv.end == NV_PAGE_SIZE);
    budget = -1;
    erases_before = erases;
    CHECK(nv_write(&nv, 2, 0, (const uint8_t *)cut, sizeof cut));
    CHECK(erases == erases_before + 1);
    uint16_t len = 0;
    const uint8_t *d1 = nv_find(&nv, 1, 0, &len);
    CHECK(d1 && len == sizeof base && !memcmp(d1, base, sizeof base));
    const uint8_t *d2 = nv_find(&nv, 2, 0, &len);
    CHECK(d2 && len == sizeof cut && !memcmp(d2, cut, sizeof cut));
}

static bool dump_visit(void *arg, uint16_t item, uint16_t sub, const uint8_t *d, uint16_t len)
{
    (void)arg;
    printf("%u %u ", item, sub);
    for (uint16_t i = 0; i < len; i++) {
        printf("%02x", d[i]);
    }
    printf("\n");
    return true;
}

// test_nvstore --dump PAGE: the current records as nvstore reads them; --write OUT: a page the gateway code writes
static int cross(const char *mode, const char *path)
{
    if (!strcmp(mode, "--dump")) {
        load_page(path);
        nvstore_t nv = store();
        nv_each(&nv, dump_visit, NULL);
        return 0;
    }
    gateway_t gw;
    erase(NULL);
    nvstore_t nv = store();
    gw_init(&gw, 0x65FE, 0xD425, 0x00124B0012345678ull, (const uint8_t *)"77A85A36", &io);
    gw_load(&gw, &nv);
    gw.pan = 0x65FE, gw.short_addr = 0xD425, gw.ext = 0x00124B0012345678ull;
    bool ok = gw_save_network(&gw) && gw_add_sensor(&gw, (const uint8_t *)"77A8C793", 0x0005) &&
              gw_add_sensor(&gw, (const uint8_t *)"777B42DF", 0x0006);
    FILE *f = fopen(path, "wb");
    ok = ok && f && fwrite(flash, 1, sizeof flash, f) == sizeof flash;
    if (f) {
        fclose(f);
    }
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && (!strcmp(argv[1], "--dump") || !strcmp(argv[1], "--write"))) {
        return cross(argv[1], argv[2]);
    }
    if (argc != 3) {
        fprintf(stderr, "usage: %s v1.4/FLASH.bin v1.8/FLASH.bin\n", argv[0]);
        return 2;
    }
    FILE *v14 = fopen(argv[1], "rb"), *v18 = fopen(argv[2], "rb");
    if (v14 && v18) {
        test_stock_pages(argv[1], argv[2]);
    } else {
        printf("stock pages skipped: the dongle dumps are not published (set OWS_GROUND_TRUTH to a full copy)\n");
    }
    if (v14) {
        fclose(v14);
    }
    if (v18) {
        fclose(v18);
    }
    test_write_and_compact();
    test_gateway_roundtrip();
    test_power_cuts();
    test_any_page_boots();
    test_large_compaction_and_guards();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
