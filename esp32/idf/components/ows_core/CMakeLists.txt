# The portable cores shared with the PC builds (esp32/CMakeLists.txt): the relay, its socket link, the outage buffer
# with the gateway's host-protocol code, the contact's bootloader, the web uploads' multipart parser.
idf_component_register(SRCS "../../../relay/relay.c" "../../../relay/sock_posix.c" "../../../relay/buffer.c"
                            "../../../../firmware/common/host_proto.c" "../../../contact/sbl.c"
                            "../../../contact/sblprobe.c" "../../../contact/gwsync.c" "../../../contact/gwnv.c"
                            "../../../contact/cjtag.c" "../../../web/multipart.c" "../../../web/http_auth.c"
                            "../../../web/mdns_pkt.c" "../../../rig/rig.c"
                       INCLUDE_DIRS "../../../relay" "../../../contact" "../../../web" "../../../rig"
                                    "../../../../firmware/common"
                       REQUIRES lwip)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
