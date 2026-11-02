// The board's CCFG once more, as ordinary read-only data (ccfg_builtin()): its ccfg.c compiled a second time, the
// structure renamed and moved out of the .ccfg section. On the dongle our image is written without its .ccfg
// (the CCFG page is left alone), so this copy is the only one of ours there.
#define __ccfg        ows_ccfg_builtin
#define section(name) section(".rodata.ows_ccfg_builtin")
#include "ccfg.c"
