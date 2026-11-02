// This image's build info block (common/build_info.h), placed right after the vector table (.ows_info); its image ID
// is zero here and written after the link by tools/image_id.py. OWS_PRODUCT, OWS_APP, OWS_VERSION (at most 39
// characters) come from firmware/CMakeLists.txt.
#include "build_info.h"

#include "board_config.h"

#ifndef OWS_DEBUG
#define OWS_DEBUG 0
#endif
#ifndef OWS_HAS_TX
#define OWS_HAS_TX 1
#endif

__attribute__((section(".ows_info"), used)) const build_info_t build_info = {
    .magic = BUILD_INFO_MAGIC,
    .format = BUILD_INFO_FORMAT,
    .flags = (OWS_HAS_TX ? BUILD_INFO_TX : 0) | (OWS_DEBUG ? BUILD_INFO_DEBUG : 0),
    .product = OWS_PRODUCT,
    .board = BOARD_NAME,
    .app = OWS_APP,
    .version = OWS_VERSION,
};
