// Records in one flash page, in our own format (docs/design/nv-format.md, issue #61; reference/wyzesense_ref/nvows.py):
// the page starts with "OWNV" and the format version; each record is a 12-byte header {A5, state (FF live, 00
// deleted), item, sub-ID, length, CRC-32 of item..data} then its data, read forward. A record is replaced by
// appending the new one and clearing the old one's state (flash clears bits without an erase); a full page is
// compacted (erased, the live records written back). A page in any other format (stock, erased, garbage) is
// formatted: stock data is carried over by the flashing/sync tools (tools/nv_json.py, tools/gwsync.py), not here.
// Power cuts (#38, the user's rule): data may be lost, a start never fails. A record whose CRC fails is invalid and
// skipped; reading stops at a header that is not one; appending never writes over a cut record's bytes; compaction
// keeps the newest records when they do not all fit. test_nvstore cuts the power at every flash operation of a
// write, a compaction and a delete, and tries random pages; test_apps starts both apps on broken pages.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NV_PAGE_SIZE      4096
#define NV_MAX_DATA       64  // longest record we write
#define NV_FORMAT_VERSION 1

typedef struct {
    const uint8_t *page;                                                           // the page, readable
    bool (*program)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);  // clears bits only
    bool (*erase)(void *ctx);                                                      // the whole page to 0xFF
    void *ctx;
    uint32_t end;  // offset after the last record
} nvstore_t;

// Finds the end of the records (formats the page if it is not in our OWNV format).
bool nv_open(nvstore_t *nv);

// The valid record (item, sub): its data and length; NULL if none.
const uint8_t *nv_find(const nvstore_t *nv, uint16_t item, uint16_t sub, uint16_t *len);

// Visits every valid record, newest first; stops when visit returns false.
void nv_each(const nvstore_t *nv,
             bool (*visit)(void *arg, uint16_t item, uint16_t sub, const uint8_t *data, uint16_t len), void *arg);

// Writes (item, sub), replacing the valid one. len: a multiple of 4, <= NV_MAX_DATA.
bool nv_write(nvstore_t *nv, uint16_t item, uint16_t sub, const uint8_t *data, uint16_t len);

// Deletes (item, sub) if present.
bool nv_delete(nvstore_t *nv, uint16_t item, uint16_t sub);
