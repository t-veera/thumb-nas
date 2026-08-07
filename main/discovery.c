#include "discovery.h"

#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "discovery";

esp_err_t discovery_start(void)
{
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
    return err;
  }

  err = mdns_hostname_set(DISCOVERY_HOSTNAME);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
    mdns_free();
    return err;
  }

  err = mdns_instance_name_set("thumb-nas");
  if (err != ESP_OK) {
    /* Cosmetic only -- this is the friendly name shown in network browsers.
       Not worth tearing down a working hostname over. */
    ESP_LOGW(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
  }

  /* Advertise the WebDAV endpoint. _davs._tcp is the registered service type
     for WebDAV over TLS; the plain-HTTP CA endpoint is deliberately not
     advertised, since it is a bootstrap step rather than a service to browse. */
  mdns_txt_item_t txt[] = {
      {"path", "/"},
      {"u",    "see SETUP.md"},
  };
  err = mdns_service_add(NULL, "_davs", "_tcp", 443, txt,
                         sizeof(txt) / sizeof(txt[0]));
  if (err != ESP_OK) {
    /* Service discovery is a convenience; name resolution is the requirement.
       Keep going so the hostname still works. */
    ESP_LOGW(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
  }

  ESP_LOGI(TAG, "mDNS up -- reachable at https://%s/", DISCOVERY_FQDN);
  return ESP_OK;
}
