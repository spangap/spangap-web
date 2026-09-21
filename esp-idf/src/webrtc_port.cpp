/**
 * webrtc_port (chip) — the platform's answers for the DataChannel.
 *
 * The host's are in `host/webrtc_port.cpp`; see `include/webrtc_port.h` for
 * what each one is for.
 */
#include "webrtc_port.h"

#include "esp_netif.h"
#include "esp_rom_crc.h"
#include <lwip/sockets.h>

extern "C" int webrtcLocalIps(char ips[][16], int max)
{
    int n = 0;
    static const char* const keys[] = { "WIFI_STA_DEF", "WIFI_AP_DEF" };
    for (const char* key : keys) {
        if (n >= max) break;
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey(key);
        if (!netif) continue;
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
            esp_ip4addr_ntoa(&info.ip, ips[n], 16);
            n++;
        }
    }
    return n;
}

extern "C" uint32_t webrtcBindAddrV4(void)
{
    return INADDR_ANY;
}

extern "C" uint32_t webrtcCrc32(const uint8_t* data, size_t len)
{
    return esp_rom_crc32_le(0, data, len);
}
