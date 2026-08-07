/*
  mDNS advertisement, so the device is reachable at a stable name regardless of
  what DHCP hands it. SRS-FRM-06.

  The hostname here is not free choice: the TLS leaf certificate's primary SAN
  is "keychain-nas.local" (see tools/gencert.py), and mDNS appends ".local"
  itself. So the hostname passed to mdns_hostname_set must be exactly
  "keychain-nas" -- anything else resolves to a name the certificate does not
  cover, and TLS validation fails even though mDNS itself works.
*/
#pragma once

#include "esp_err.h"

/* The bare mDNS hostname, without the .local suffix that mDNS appends. */
#define DISCOVERY_HOSTNAME "keychain-nas"

/* Full name clients use. Must match the certificate SAN exactly. */
#define DISCOVERY_FQDN     DISCOVERY_HOSTNAME ".local"

/* Starts mDNS and advertises the WebDAV service. Safe to call once WiFi has
   an IP. Returns ESP_OK on success. */
esp_err_t discovery_start(void);
