# OTA slot images' version (#57): a uint32 that a sensor compares unsigned ("newer" = higher, docs/design/ota.md), encoded from
# the release tag so that every release is above every older build, the archive repository's included (whose images
# were numbered by commit count, up to a few hundred):
#
#     major << 24 | minor << 16 | patch << 8 | commits since the tag
#
# from git describe ("v0.2.1", "v0.2.1-3-gabcdef0", either with "-dirty"). Each release field must be 0-255 (else
# the build fails); the commit count saturates at 255 with a warning, so it never carries into the patch byte (builds
# more than 255 commits past a tag then share one version: tag a release). A pre-release tag (v0.3.0-rc1) has no
# place in this order (it would equal its final release): refused, OWS_IMAGE_VERSION sets a number instead. Without a
# tag (a clone without tags): 0.0.0 and the commit count, saturated, below every release.
#
#   ows_image_version(<describe> <commit count> <out var>)
#   cmake -DDESCRIBE=... [-DCOUNT=n] -P image_version.cmake   prints the version (tests: test_image_version.py)

function(ows_image_version describe count out)
    set(rx "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9]+)-g[0-9a-f]+)?(-dirty)?$")
    if(describe MATCHES "${rx}")
        set(major ${CMAKE_MATCH_1})
        set(minor ${CMAKE_MATCH_2})
        set(patch ${CMAKE_MATCH_3})
        set(n "${CMAKE_MATCH_5}")
        if(n STREQUAL "")
            set(n 0)
        endif()
    elseif(describe MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+-")
        message(FATAL_ERROR "OTA image version: '${describe}' is a pre-release tag, which has no place in the version "
                            "order (#57): set -DOWS_IMAGE_VERSION=<number>, or tag a release")
    elseif(describe MATCHES "^[0-9a-f]+(-dirty)?$" OR describe STREQUAL "")
        message(WARNING "OTA image version: no release tag in '${describe}': 0.0.0 + the commit count, below every release")
        set(major 0)
        set(minor 0)
        set(patch 0)
        set(n "${count}")
        if(n STREQUAL "")
            set(n 0)
        endif()
    else()
        message(FATAL_ERROR "OTA image version: cannot read '${describe}' (a tag vX.Y.Z expected)")
    endif()
    foreach(field major minor patch)
        if(${field} GREATER 255)
            message(FATAL_ERROR "OTA image version: ${field} ${${field}} of '${describe}' is above 255 (one byte each)")
        endif()
    endforeach()
    if(n GREATER 255)
        message(WARNING "OTA image version: ${n} commits since the tag, counted as 255 (tag a release)")
        set(n 255)
    endif()
    math(EXPR v "(${major} << 24) | (${minor} << 16) | (${patch} << 8) | ${n}")
    set(${out} ${v} PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if(NOT DEFINED COUNT)
        set(COUNT 0)
    endif()
    ows_image_version("${DESCRIBE}" "${COUNT}" v)
    message("${v}")
endif()
