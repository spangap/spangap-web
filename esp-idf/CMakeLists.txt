file(GLOB WEB_SRCS "src/*.cpp")

# The WebRTC DataChannel builds on both targets: ICE, DTLS, SCTP and the router
# above them are the same source, so a browser talking to a simulated station
# talks to it exactly as it talks to a board. Only `webrtc_port` differs — the
# local addresses to advertise, the address the UDP socket binds, and a CRC32
# the chip has in ROM.
set(_WEB_IDF_REQ esp_event mbedtls json)
if(IDF_TARGET STREQUAL "linux")
    list(REMOVE_ITEM WEB_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/webrtc_port.cpp")
    list(APPEND WEB_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/host/webrtc_port.cpp")
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
