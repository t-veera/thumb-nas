#include "cacert_server.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "cacert";

/* Embedded by EMBED_TXTFILES in main/CMakeLists.txt. This is the CA's public
   certificate only -- never ca.key, which stays on the developer's machine. */
extern const uint8_t ca_crt_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_crt_end[]   asm("_binary_ca_crt_end");

/* GET /ca.crt
   Unauthenticated by necessity: a client that does not yet trust the CA cannot
   complete a TLS handshake, so it has no way to reach the HTTPS server to
   collect it. Serving the public half of a certificate is not a disclosure --
   it is published to every client during every TLS handshake anyway. */
static esp_err_t ca_crt_get_handler(httpd_req_t *req)
{
  size_t len = ca_crt_end - ca_crt_start;

  /* Trim the NUL that EMBED_TXTFILES appends; it is not part of the PEM. */
  if (len > 0 && ca_crt_start[len - 1] == '\0') {
    len--;
  }

  ESP_LOGI(TAG, "served /ca.crt (%u bytes)", (unsigned)len);

  /* This MIME type makes Windows and Android offer to install the certificate
     rather than rendering it as text. */
  httpd_resp_set_type(req, "application/x-x509-ca-cert");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"ca.crt\"");
  return httpd_resp_send(req, (const char *)ca_crt_start, len);
}

httpd_handle_t cacert_server_start(void)
{
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  config.server_port = 80;

  /* Each httpd instance needs its own control socket, or httpd_start fails.
     Verified in the headers rather than assumed:
       esp_http_server.h:24   ESP_HTTPD_DEF_CTRL_PORT = 32768
       esp_https_server.h:165 HTTPD_SSL_CONFIG_DEFAULT uses DEF_CTRL_PORT + 1
     So 32768 and 32769 are both taken by the WebDAV server. Use the next one. */
  config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 2;

  /* Room for exactly one route, and no wildcard matcher. Both are deliberate:
     uri_match_fn stays NULL so matching is exact, which means a request for
     anything other than /ca.crt cannot fall through to a handler. */
  config.max_uri_handlers = 1;
  config.uri_match_fn     = NULL;
  config.stack_size       = 4096;
  config.lru_purge_enable = true;

  ESP_LOGI(TAG, "Starting CA distribution server (plain HTTP) on port %d",
           config.server_port);

  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start CA distribution server");
    return NULL;
  }

  const httpd_uri_t ca_crt = {
      .uri      = "/ca.crt",
      .method   = HTTP_GET,
      .handler  = ca_crt_get_handler,
      .user_ctx = NULL,
  };

  if (httpd_register_uri_handler(server, &ca_crt) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register /ca.crt handler");
    httpd_stop(server);
    return NULL;
  }

  return server;
}
