file(GLOB WEB_SRCS "src/*.cpp")

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
        esp_event
        esp_netif
        lwip
        mbedtls
        json
)
