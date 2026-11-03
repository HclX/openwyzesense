#include "nvstore.h"

#include <string.h>

#include "mac154.h"

// docs/design/nv-format.md
#define PAGE_HDR 8
#define REC      12
#define MARKER   0xA5
#define LIVE     0xFF
#define DELETED  0x00

static const uint8_t page_header[PAGE_HDR] = {'O', 'W', 'N', 'V', NV_FORMAT_VERSION, 0xFF, 0xFF, 0xFF};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t record_crc(uint16_t item, uint16_t sub, const uint8_t *data, uint16_t len)
{
    uint8_t b[6 + NV_MAX_DATA];
    b[0] = (uint8_t)item, b[1] = (uint8_t)(item >> 8);
    b[2] = (uint8_t)sub, b[3] = (uint8_t)(sub >> 8);
    b[4] = (uint8_t)len, b[5] = (uint8_t)(len >> 8);
    memcpy(b + 6, data, len);
    return crc32_ieee(b, 6 + (size_t)len);
}

typedef struct {
    uint32_t at;  // the record's offset
    uint16_t item, sub, len;
    bool live;  // state LIVE
    bool good;  // its CRC holds
} rec_t;

// The record at *at, and *at moved past it. False at the end of the records: the page's end, an unwritten marker,
// or a header that is not one (no length to skip by). A record whose CRC fails comes back with good false: it is
// invalid, the ones after it still count.
static bool next(const uint8_t *page, uint32_t *at, rec_t *r)
{
    const uint8_t *h = page + *at;
    if (*at + REC > NV_PAGE_SIZE || h[0] != MARKER) {
        return false;
    }
    r->at = *at;
    r->item = le16(h + 2), r->sub = le16(h + 4), r->len = le16(h + 6);
    if (r->len % 4 || r->len > NV_MAX_DATA || *at + REC + r->len > NV_PAGE_SIZE) {
        return false;
    }
    r->live = h[1] == LIVE;
    r->good = le32(h + 8) == record_crc(r->item, r->sub, h + REC, r->len);
    *at += REC + r->len;
    return true;
}

// The value of (item, sub): a good, live record with no good live copy of it after it (a power cut between
// appending and invalidating leaves two: the newer wins)
static bool current(const uint8_t *page, const rec_t *r)
{
    uint32_t at = PAGE_HDR;
    rec_t later;
    if (!r->good || !r->live) {
        return false;
    }
    while (next(page, &at, &later)) {
        if (later.at > r->at && later.good && later.live && later.item == r->item && later.sub == r->sub) {
            return false;
        }
    }
    return true;
}

static bool format(nvstore_t *nv)
{
    if (!nv->erase(nv->ctx) || !nv->program(nv->ctx, 0, page_header, PAGE_HDR)) {
        nv->end = NV_PAGE_SIZE;
        return false;
    }
    nv->end = PAGE_HDR;
    return true;
}

static bool compact(nvstore_t *nv, uint16_t skip_item, uint16_t skip_sub, size_t reserve);

bool nv_open(nvstore_t *nv)
{
    if (memcmp(nv->page, page_header, PAGE_HDR) != 0) {  // stock, erased, garbage: not ours (the tools migrate)
        return format(nv);
    }
    uint32_t at = PAGE_HDR;
    rec_t r;
    while (next(nv->page, &at, &r)) {
    }
    for (uint32_t i = at; i < NV_PAGE_SIZE; i++) {
        if (nv->page[i] != 0xFF) {  // something after the records (a write cut in its header): a clean page again
            return compact(nv, 0xFFFF, 0xFFFF, 0);
        }
    }
    nv->end = at;  // appending goes right after the records
    return true;
}

void nv_each(const nvstore_t *nv, bool (*visit)(void *, uint16_t, uint16_t, const uint8_t *, uint16_t), void *arg)
{
    uint32_t at = PAGE_HDR;
    rec_t r;
    while (next(nv->page, &at, &r)) {
        if (current(nv->page, &r) && !visit(arg, r.item, r.sub, nv->page + r.at + REC, r.len)) {
            return;
        }
    }
}

typedef struct {
    uint16_t item, sub, len;
    const uint8_t *data;
} find_t;

static bool find_visit(void *arg, uint16_t item, uint16_t sub, const uint8_t *data, uint16_t len)
{
    find_t *f = arg;
    if (item != f->item || sub != f->sub) {
        return true;
    }
    f->data = data;
    f->len = len;
    return false;
}

const uint8_t *nv_find(const nvstore_t *nv, uint16_t item, uint16_t sub, uint16_t *len)
{
    find_t f = {item, sub, 0, NULL};
    nv_each(nv, find_visit, &f);
    if (f.data && len) {
        *len = f.len;
    }
    return f.data;
}

// Clears the state of every live (item, sub) that starts before `before`.
static bool invalidate(nvstore_t *nv, uint16_t item, uint16_t sub, uint32_t before)
{
    static const uint8_t deleted = DELETED;
    uint32_t at = PAGE_HDR;
    rec_t r;
    while (next(nv->page, &at, &r) && r.at < before) {
        if (r.live && r.item == item && r.sub == sub && !nv->program(nv->ctx, r.at + 1, &deleted, 1)) {
            return false;
        }
    }
    return true;
}

static bool append(nvstore_t *nv, uint16_t item, uint16_t sub, const uint8_t *data, uint16_t len)
{
    uint32_t crc = record_crc(item, sub, data, len);
    uint8_t h[REC] = {MARKER,
                      LIVE,
                      (uint8_t)item,
                      (uint8_t)(item >> 8),
                      (uint8_t)sub,
                      (uint8_t)(sub >> 8),
                      (uint8_t)len,
                      (uint8_t)(len >> 8),
                      (uint8_t)crc,
                      (uint8_t)(crc >> 8),
                      (uint8_t)(crc >> 16),
                      (uint8_t)(crc >> 24)};
    if (nv->end + REC + len > NV_PAGE_SIZE) {
        return false;
    }
    // the header first: cut before the data is complete, the CRC fails and the record is ignored
    if (!nv->program(nv->ctx, nv->end, h, REC) || (len && !nv->program(nv->ctx, nv->end + REC, data, len))) {
        nv->end = NV_PAGE_SIZE;  // dirty tail: force compaction before any subsequent write
        return false;
    }
    nv->end += REC + len;
    return true;
}

// Erases the page and writes all current records back, oldest first, without (skip_item, skip_sub) (about to be
// written). Aborts BEFORE erasing the page if live records + `reserve` exceed page capacity so no live record is
// ever silently truncated; `keep` lives on the stack instead of permanent `.bss`.
__attribute__((noinline)) static bool compact(nvstore_t *nv, uint16_t skip_item, uint16_t skip_sub, size_t reserve)
{
    uint8_t keep[NV_PAGE_SIZE - PAGE_HDR];
    size_t used = 0;
    uint32_t at = PAGE_HDR;
    rec_t r;
    while (next(nv->page, &at, &r)) {
        if (!current(nv->page, &r) || (r.item == skip_item && r.sub == skip_sub)) {
            continue;
        }
        size_t sz = REC + (size_t)r.len;
        if (used + sz > sizeof keep || PAGE_HDR + used + sz + reserve > NV_PAGE_SIZE) {
            return false;
        }
        memcpy(keep + used, nv->page + r.at, sz);
        used += sz;
    }
    if (!format(nv)) {
        return false;
    }
    for (size_t k = 0; k < used;) {
        const uint8_t *h = keep + k;
        uint16_t len = le16(h + 6);
        if (!append(nv, le16(h + 2), le16(h + 4), h + REC, len)) {
            return false;
        }
        k += REC + (size_t)len;
    }
    return true;
}

bool nv_write(nvstore_t *nv, uint16_t item, uint16_t sub, const uint8_t *data, uint16_t len)
{
    if (len % 4 || len > NV_MAX_DATA) {
        return false;
    }
    if (nv->end + REC + len > NV_PAGE_SIZE) {
        return compact(nv, item, sub, REC + (size_t)len) && append(nv, item, sub, data, len);
    }
    // the new record first: if power fails between the two steps, the newest copy wins on reading
    uint32_t at = nv->end;
    return append(nv, item, sub, data, len) && invalidate(nv, item, sub, at);
}

bool nv_delete(nvstore_t *nv, uint16_t item, uint16_t sub)
{
    return invalidate(nv, item, sub, NV_PAGE_SIZE);
}
