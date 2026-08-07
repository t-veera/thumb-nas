/*
  Waveshare ESP32-S3-Touch-AMOLED-2.06 -- SD card smoke test
  Pure ESP-IDF, no Arduino core.
  -----------------------------------------------------------
  Built from Espressif's own official examples:
    - examples/storage/sd_card/sdspi  (SD-over-SPI mount)
    - examples/wifi/getting_started/station  (WiFi connect)
    - esp_http_server component  (serving the test file)

  TF card pins, per Waveshare's documentation for this board (SPI mode --
  only 4 signals are documented for this board's TF slot, which is SPI,
  not the 6-signal 4-bit SDMMC bus):
    CS   = GPIO17
    MOSI = GPIO1
    MISO = GPIO3
    SCK  = GPIO2

  Before building:
    1. Format the TF card FAT32 (not exFAT) if it's above 32GB.
    2. Copy main/wifi_credentials.h.example to main/wifi_credentials.h and
       fill in your SSID and password there. That file is gitignored.

  Build / flash / monitor (from this project's root folder):
    idf.py set-target esp32s3
    idf.py build
    idf.py -p <PORT> flash monitor

  On success, the serial log prints the board's IP address and the exact
  URL to fetch /test.md from your Windows or Linux laptop.
*/

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "webdav.h"
#include "cacert_server.h"

static const char *TAG = "waveshare_test";

/* WiFi credentials live in wifi_credentials.h, which is gitignored so the
   password never reaches a commit. Copy wifi_credentials.h.example to
   wifi_credentials.h and fill it in. */
#include "wifi_credentials.h"

#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD)
#error "Missing WiFi credentials -- copy main/wifi_credentials.h.example to main/wifi_credentials.h and fill in your network details."
#endif

#define MOUNT_POINT "/sdcard"

// Waveshare ESP32-S3-Touch-AMOLED-2.06 documented TF card SPI pins
#define PIN_NUM_MISO 3
#define PIN_NUM_MOSI 1
#define PIN_NUM_CLK  2
#define PIN_NUM_CS   17

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

// ---------------- WiFi ----------------

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < 10) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry connecting to AP (%d/10)", s_retry_num);
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static bool wifi_init_sta(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

  wifi_config_t wifi_config = {
      .sta = {
          .ssid = WIFI_SSID,
          .password = WIFI_PASSWORD,
      },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Connecting to %s ...", WIFI_SSID);

  /* This timeout must outlast the retry budget above, or it fires first and we
     give up while the connection is still in progress. Ten retries at roughly
     2.4s each is ~24s, so a 20s timeout used to lose the race on a weak
     signal: app_main returned without starting any server, and the IP then
     arrived a second later, leaving the board reachable but serving nothing.
     WIFI_FAIL_BIT is the intended failure path; this is only a backstop. */
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to %s", WIFI_SSID);
    return true;
  }
  ESP_LOGE(TAG, "Failed to connect to %s -- check SSID/password", WIFI_SSID);
  return false;
}

// ---------------- SD card (SPI) ----------------

static esp_err_t sd_card_init(void) {
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024,
  };
  sdmmc_card_t *card;
  const char mount_point[] = MOUNT_POINT;

  ESP_LOGI(TAG, "Initializing SD card over SPI");
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();

  spi_bus_config_t bus_cfg = {
      .mosi_io_num = PIN_NUM_MOSI,
      .miso_io_num = PIN_NUM_MISO,
      .sclk_io_num = PIN_NUM_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4000,
  };
  esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SPI bus (%s)", esp_err_to_name(ret));
    return ret;
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = PIN_NUM_CS;
  slot_config.host_id = host.slot;

  ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount filesystem. Card may need FAT32 formatting.");
    } else {
      ESP_LOGE(TAG, "Failed to init SD card (%s). Check wiring / pull-ups on CS, MOSI, MISO.", esp_err_to_name(ret));
    }
    return ret;
  }

  ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
  sdmmc_card_print_info(stdout, card);
  return ESP_OK;
}

static esp_err_t write_test_file(void) {
  const char *path = MOUNT_POINT "/test.md";
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open %s for writing", path);
    return ESP_FAIL;
  }
  fprintf(f, "# Keychain NAS -- SD Card Test\n\n");
  fprintf(f, "Written by the Waveshare ESP32-S3-Touch-AMOLED-2.06 (ESP-IDF, no Arduino) at boot.\n");
  fprintf(f, "Free heap at write time: %lu bytes\n", (unsigned long)esp_get_free_heap_size());
  fclose(f);
  ESP_LOGI(TAG, "Wrote %s", path);
  return ESP_OK;
}

// ---------------- app_main ----------------

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  bool sd_ok = (sd_card_init() == ESP_OK);
  if (sd_ok) {
    write_test_file();
  } else {
    ESP_LOGE(TAG, "SD card init failed -- WiFi will still come up so you can see this log, but /test.md will not exist.");
  }

  if (wifi_init_sta()) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);

    ESP_LOGI(TAG, "--------------------------------------------------");
    ESP_LOGI(TAG, "STEP 1, get the certificate:  http://" IPSTR "/ca.crt", IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "STEP 2, mount the drive:      net use Z: https://" IPSTR "/ *",
             IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Or fetch a file directly:     curl --cacert ca.crt -u %s https://" IPSTR "/test.md",
             WEBDAV_USER, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "--------------------------------------------------");

    /* Started regardless of the SD card: a client still needs the CA to reach
       the HTTPS server at all, and this listener never touches /sdcard. */
    cacert_server_start();

    if (sd_ok) {
      webdav_start(MOUNT_POINT);
    } else {
      ESP_LOGE(TAG, "Not starting HTTP server -- no SD card mounted, nothing to serve.");
    }
  }
}
