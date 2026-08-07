/*
  A deliberately tiny plain-HTTP listener whose only job is to hand out the CA
  certificate so a new client can bootstrap trust.

  This is kept in its own translation unit, with its own server handle and its
  own single registered route, so that the unencrypted listener can never
  accidentally inherit a WebDAV handler. Nothing here touches /sdcard.
*/
#pragma once

#include "esp_http_server.h"

/* Starts the plain-HTTP CA distribution server on port 80.
   Serves exactly one path: GET /ca.crt. Everything else is 404.
   Returns NULL on failure. */
httpd_handle_t cacert_server_start(void);
