/*
  Minimal WebDAV server over ESP-IDF's own esp_http_server.

  Verified against ESP-IDF v5.5.2 before writing:
    - httpd_method_t is "typedef enum http_method" (esp_http_server.h:115),
      so http_parser's WebDAV verbs are directly usable:
      PROPFIND=12, MKCOL=10, MOVE=11, COPY=8, LOCK=9, UNLOCK=15.
    - httpd_parse.c:69 rejects only negative method values -- there is no
      whitelist, so custom verbs reach registered handlers.
    - httpd_uri_match_wildcard (esp_http_server.h:1079) assigned to
      httpd_config_t.uri_match_fn lets one handler per method serve any path.
*/
#pragma once

#include "esp_http_server.h"

/* Starts the WebDAV server, serving the filesystem rooted at base_path
   (e.g. "/sdcard"). Returns NULL on failure. */
httpd_handle_t webdav_start(const char *base_path);
