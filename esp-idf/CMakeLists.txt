file(GLOB WEB_SRCS "src/*.cpp")

# webrtc_task asks upnp for the external IP (gated by CONFIG_SPANGAP_UPNP);
# REQUIRES upnp at the CMake level for header visibility. Plan-aligned
# future inverts this (upnp's browser/lcd panels decorate web), but for
# now the direct call survives.
idf_component_register(
    SRCS ${WEB_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        spangap-core
        spangap-net
        spangap-lcd
        upnp
        esp_event
        esp_netif
        lwip
        mbedtls
        json
)
