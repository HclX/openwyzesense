// The NV page: on a CC1310, flash page 30 (0x1E000), in our OWNV record format (common/nvstore.h; a stock dongle's
// page 30 is migrated by tools/nv_json.py or tools/gwsync.py).
#pragma once

#include "nvstore.h"

// An nvstore on the page, opened (formatted if not already in our OWNV format).
bool nv_page_open(nvstore_t *nv);
