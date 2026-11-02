// The host board's build info block (common/build_info.h): what the host builds of the apps answer to
// HOST_OWS_BUILD_INFO. Its ID stays zero (no flash image to hash).
#include "build_info.h"

#include "board_config.h"

const build_info_t build_info = {
    .magic = BUILD_INFO_MAGIC,
    .format = BUILD_INFO_FORMAT,
    .flags = BUILD_INFO_TX | BUILD_INFO_DEBUG,  // the host builds have both (firmware/CMakeLists.txt)
    .product = "host",
    .board = BOARD_NAME,
    .app = "host",
    .version = "host",
};
