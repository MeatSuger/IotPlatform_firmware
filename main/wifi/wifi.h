#ifndef _WIFI_H_
#define _WIFI_H_

#include <stdbool.h>

/* --------------------------------------------------------------------------
 * WiFi STA management: init, connect, event signaling.
 *
 * On success the WIFI_CONNECTED_BIT bit is set on g_wifiEventGroup
 * (see common.h). The event handler keeps reconnecting automatically
 * while the STA is enabled.
 * -------------------------------------------------------------------------- */

/* Initialize netif/event loop, start STA and block until connected or
 * 15 s timeout. Returns true when connected. */
bool wifi_init_sta(void);

#endif /* _WIFI_H_ */
