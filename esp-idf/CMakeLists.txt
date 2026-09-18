file(GLOB WEB_SRCS "src/*.cpp")

# The WebRTC DataChannel is an lwIP UDP stack with its own SCTP on top; the
# host build serves the browser over TCP and leaves it out, keeping only the
# boot hook the generated dispatch calls.
set(_WEB_IDF_REQ esp_event mbedtls json)
if(IDF_TARGET STREQUAL "linux")
    list(REMOVE_ITEM WEB_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/webrtc_task.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/webrtc_sctp.cpp")
    list(APPEND WEB_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/host/webrtc_stub.cpp")
else()
    list(APPEND _WEB_IDF_REQ esp_netif lwip)
endif()

# Cross-straddle REQUIRES come through spangap_requires.cmake (auto-generated
# by spangap-inside). spangap-core/-net/upnp are declared in straddle.yaml's
# requires:. spangap-lcd is intentionally NOT listed — spangap-web has no
# direct lcd.h dep today.
include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS ${WEB_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        ${SPANGAP_REQUIRES}
        ${_WEB_IDF_REQ}
)
