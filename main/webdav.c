#include "webdav.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_https_server.h"
#include "mbedtls/base64.h"
#include "esp_vfs_fat.h"
#include "wifi_credentials.h"

#if !defined(WEBDAV_USER) || !defined(WEBDAV_PASS)
#error "Missing WebDAV credentials -- add WEBDAV_USER and WEBDAV_PASS to main/wifi_credentials.h (see the .example)."
#endif

static const char *TAG = "webdav";

#define DAV_MAX_PATH 512
#define DAV_SCRATCH  512

/* Filesystem root this server exposes, e.g. "/sdcard". */
static char s_base_path[64];

/* ---------------------------------------------------------------------------
   HTTP Basic authentication

   esp_http_server has no global pre-handler hook in this IDF version --
   open_fn fires per connection, not per request, and uri_match_fn sees no
   headers. So every handler guards itself with DAV_REQUIRE_AUTH.

   Basic auth is base64, not encryption. This is only defensible because the
   server is HTTPS-only; over plain HTTP the password would be readable on the
   wire by anyone.
   --------------------------------------------------------------------------- */

/* Length-independent comparison, so response time does not leak how many
   leading characters of the password were correct. */
static bool secure_equals(const char *a, const char *b)
{
  size_t la = strlen(a), lb = strlen(b);
  unsigned char diff = (unsigned char)(la ^ lb);
  size_t n = la < lb ? la : lb;
  for (size_t i = 0; i < n; i++) {
    diff |= (unsigned char)(a[i] ^ b[i]);
  }
  return diff == 0;
}

static esp_err_t send_401(httpd_req_t *req)
{
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Keychain NAS\"");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;  /* a valid HTTP response, not a server error */
}

static bool check_auth(httpd_req_t *req)
{
  size_t hlen = httpd_req_get_hdr_value_len(req, "Authorization");
  if (hlen == 0 || hlen > 256) {
    send_401(req);
    return false;
  }

  char hdr[264];
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
    send_401(req);
    return false;
  }

  if (strncasecmp(hdr, "Basic ", 6) != 0) {
    send_401(req);
    return false;
  }

  const char *b64 = hdr + 6;
  while (*b64 == ' ') {
    b64++;
  }

  unsigned char decoded[192];
  size_t olen = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &olen,
                            (const unsigned char *)b64, strlen(b64)) != 0) {
    send_401(req);
    return false;
  }
  decoded[olen] = '\0';

  /* "user:pass" -- the password may itself contain ':', so split on the first
     one only. */
  char *sep = strchr((char *)decoded, ':');
  if (!sep) {
    send_401(req);
    return false;
  }
  *sep = '\0';
  const char *user = (const char *)decoded;
  const char *pass = sep + 1;

  bool ok = secure_equals(user, WEBDAV_USER) && secure_equals(pass, WEBDAV_PASS);

  /* Scrub the decoded credentials off the stack rather than leaving them for
     whatever reuses this frame. */
  memset(decoded, 0, sizeof(decoded));

  if (!ok) {
    ESP_LOGW(TAG, "auth failed for %s", req->uri);
    send_401(req);
    return false;
  }
  return true;
}

/* One line per handler, since there is no global hook to register instead. */
#define DAV_REQUIRE_AUTH(req)              \
  do {                                     \
    if (!check_auth(req)) return ESP_OK;   \
  } while (0)

/* ---------------------------------------------------------------------------
   URI <-> filesystem path helpers
   --------------------------------------------------------------------------- */

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Percent-decode src into dst. WebDAV clients encode spaces and non-ASCII in
   hrefs, so the raw URI cannot be used as a path directly. */
static void uri_decode(char *dst, size_t dst_len, const char *src)
{
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 1 < dst_len; i++) {
    if (src[i] == '%' && src[i + 1] && src[i + 2]) {
      int hi = hexval(src[i + 1]), lo = hexval(src[i + 2]);
      if (hi >= 0 && lo >= 0) {
        dst[o++] = (char)((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    dst[o++] = src[i];
  }
  dst[o] = '\0';
}

/* Percent-encode the characters that must not appear raw in an href. */
static void uri_encode(char *dst, size_t dst_len, const char *src)
{
  static const char *hex = "0123456789ABCDEF";
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 1 < dst_len; i++) {
    unsigned char c = (unsigned char)src[i];
    bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || strchr("-_.~/", c) != NULL;
    if (safe) {
      dst[o++] = (char)c;
    } else if (o + 3 < dst_len) {
      dst[o++] = '%';
      dst[o++] = hex[c >> 4];
      dst[o++] = hex[c & 0x0f];
    } else {
      break;
    }
  }
  dst[o] = '\0';
}

/* Escape the five XML predefined entities. Filenames on a FAT card can legally
   contain '&', which would otherwise produce malformed multistatus XML -- the
   single most common reason a client refuses to mount. */
static void xml_escape(char *dst, size_t dst_len, const char *src)
{
  size_t o = 0;
  for (size_t i = 0; src[i]; i++) {
    const char *rep = NULL;
    switch (src[i]) {
      case '&':  rep = "&amp;";  break;
      case '<':  rep = "&lt;";   break;
      case '>':  rep = "&gt;";   break;
      case '"':  rep = "&quot;"; break;
      case '\'': rep = "&apos;"; break;
      default: break;
    }
    if (rep) {
      size_t rl = strlen(rep);
      if (o + rl + 1 >= dst_len) break;
      memcpy(dst + o, rep, rl);
      o += rl;
    } else {
      if (o + 2 >= dst_len) break;
      dst[o++] = src[i];
    }
  }
  dst[o] = '\0';
}

/* Strip any query string, decode, and map a request URI onto the SD card.
   Returns false if the result would escape the base path. */
static bool uri_to_fs(const char *uri, char *out, size_t out_len)
{
  char raw[DAV_MAX_PATH];
  size_t n = 0;
  for (; uri[n] && uri[n] != '?' && uri[n] != '#' && n + 1 < sizeof(raw); n++) {
    raw[n] = uri[n];
  }
  raw[n] = '\0';

  char decoded[DAV_MAX_PATH];
  uri_decode(decoded, sizeof(decoded), raw);

  /* Reject traversal outright rather than trying to normalise it. */
  if (strstr(decoded, "..")) {
    return false;
  }

  /* Drop a trailing slash so stat() behaves consistently, but keep root. */
  size_t dl = strlen(decoded);
  while (dl > 1 && decoded[dl - 1] == '/') {
    decoded[--dl] = '\0';
  }

  if (dl <= 1) {
    snprintf(out, out_len, "%s", s_base_path);
  } else {
    snprintf(out, out_len, "%s%s", s_base_path, decoded);
  }
  return true;
}

/* Bounded "a/b" join. Written as explicit copies rather than snprintf("%s/%s")
   because IDF builds with -Werror=format-truncation, and GCC cannot prove the
   concatenation of two same-sized buffers fits. Truncates safely if it will
   not. */
static void path_join(char *dst, size_t dst_len, const char *a, const char *b)
{
  size_t n = 0;
  for (; a[n] && n + 1 < dst_len; n++) {
    dst[n] = a[n];
  }
  if (n + 1 < dst_len && (n == 0 || dst[n - 1] != '/')) {
    dst[n++] = '/';
  }
  for (size_t i = 0; b[i] && n + 1 < dst_len; i++) {
    dst[n++] = b[i];
  }
  dst[n] = '\0';
}

/* Append a trailing slash if there is room and one is not already present.
   Collection hrefs must end in '/' or clients mis-classify them. */
static void append_slash(char *dst, size_t dst_len)
{
  size_t n = strlen(dst);
  if (n + 1 < dst_len && (n == 0 || dst[n - 1] != '/')) {
    dst[n] = '/';
    dst[n + 1] = '\0';
  }
}

/* Bounded copy. */
static void str_copy(char *dst, size_t dst_len, const char *src)
{
  size_t n = 0;
  for (; src[n] && n + 1 < dst_len; n++) {
    dst[n] = src[n];
  }
  dst[n] = '\0';
}

/* OS housekeeping files that clutter a directory listing. This is a *display*
   filter for PROPFIND only -- GET/PUT/DELETE on these paths still work if a
   client asks for one by name, which matters because Windows and macOS
   actively write some of them and would break if the writes were refused. */
static bool is_hidden_junk(const char *name)
{
  static const char *exact[] = {
      "System Volume Information",
      "$RECYCLE.BIN",
      "desktop.ini",
      "Thumbs.db",
      ".DS_Store",
  };
  for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
    if (strcasecmp(name, exact[i]) == 0) {
      return true;
    }
  }
  /* AppleDouble sidecar files: "._" followed by the real filename. */
  if (name[0] == '.' && name[1] == '_') {
    return true;
  }
  return false;
}

/* ---------------------------------------------------------------------------
   ETags

   Clients that edit-and-save need a validator: they GET a file with an ETag,
   then PUT it back with If-Match so a concurrent change is detected rather than
   silently clobbered. With no ETag to condition on, several clients refuse the
   write outright rather than risk losing someone else's edit -- which is the
   safe choice on their part, and looked like a read-only filesystem from the
   outside.

   mtime+size is sufficient here. It need not be cryptographic: this is a
   single-writer device, and the only requirement is that the value changes
   whenever the file does.
   --------------------------------------------------------------------------- */

/* Writes a quoted entity-tag, e.g. "5f2a1c-3e8". Quotes are part of the value
   per RFC 9110 and clients echo them back verbatim. */
static void compute_etag(const struct stat *st, char *out, size_t out_len)
{
  snprintf(out, out_len, "\"%llx-%llx\"",
           (unsigned long long)st->st_mtime,
           (unsigned long long)st->st_size);
}

/* Does an If-Match / If-None-Match header value select this etag?
   Handles "*", a single tag, and a comma-separated list. Weak prefixes (W/)
   are tolerated by matching on the quoted portion. */
static bool etag_list_matches(const char *header, const char *etag)
{
  while (*header == ' ') {
    header++;
  }
  if (header[0] == '*' ) {
    return true;
  }
  /* Substring search is adequate: etag is quoted, so it cannot match a partial
     token in the list. */
  return strstr(header, etag) != NULL;
}

/* Evaluates If-Match and If-None-Match against the current state of a resource.
   Returns true if the request may proceed; on false the response has already
   been sent. `exists` and `st` describe the target before the operation. */
static bool preconditions_ok(httpd_req_t *req, bool exists, const struct stat *st)
{
  char etag[48] = "";
  if (exists) {
    compute_etag(st, etag, sizeof(etag));
  }

  char hdr[192];

  /* If-None-Match: "*" means "only if it does not already exist" -- how clients
     express create-but-do-not-overwrite. */
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", hdr, sizeof(hdr)) == ESP_OK) {
    ESP_LOGI(TAG, "If-None-Match: %s (current %s)", hdr, exists ? etag : "<absent>");
    if (exists && etag_list_matches(hdr, etag)) {
      httpd_resp_set_status(req, "412 Precondition Failed");
      httpd_resp_send(req, NULL, 0);
      return false;
    }
  }

  if (httpd_req_get_hdr_value_str(req, "If-Match", hdr, sizeof(hdr)) == ESP_OK) {
    ESP_LOGI(TAG, "If-Match: %s (current %s)", hdr, exists ? etag : "<absent>");
    /* If-Match on something that no longer exists always fails. */
    if (!exists || !etag_list_matches(hdr, etag)) {
      httpd_resp_set_status(req, "412 Precondition Failed");
      httpd_resp_send(req, NULL, 0);
      return false;
    }
  }

  /* The WebDAV If: header carries lock state tokens. We do not enforce locks
     (see the LOCK section), but log it so client behaviour is visible in the
     serial trace during diagnosis. */
  if (httpd_req_get_hdr_value_str(req, "If", hdr, sizeof(hdr)) == ESP_OK) {
    ESP_LOGI(TAG, "If: %s", hdr);
  }

  return true;
}

/* RFC 1123 date, which is what getlastmodified must carry. */
static void http_date(time_t t, char *out, size_t out_len)
{
  struct tm tm_buf;
  gmtime_r(&t, &tm_buf);
  strftime(out, out_len, "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
}

/* ---------------------------------------------------------------------------
   OPTIONS
   --------------------------------------------------------------------------- */

static esp_err_t dav_options_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  ESP_LOGI(TAG, "OPTIONS %s", req->uri);

  /* Class 2 announces LOCK/UNLOCK support, which Windows checks before it will
     attempt a write. The locking behind it is a stub -- see dav_lock_handler. */
  httpd_resp_set_hdr(req, "DAV", "1,2");
  httpd_resp_set_hdr(req, "Allow",
                     "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, MOVE, LOCK, UNLOCK");
  httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* ---------------------------------------------------------------------------
   PROPFIND
   --------------------------------------------------------------------------- */

/* Emit one <D:response> block. href_path is the URI-space path (not the
   filesystem path) and must already be percent-encoded. */
static esp_err_t send_response_block(httpd_req_t *req, const char *href_path,
                                     bool is_dir, off_t size, time_t mtime)
{
  char datebuf[40];
  http_date(mtime, datebuf, sizeof(datebuf));

  /* Same derivation as compute_etag() -- kept consistent so the value a client
     reads from PROPFIND is the one it can later send back in If-Match. */
  char etag[48];
  snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
           (unsigned long long)mtime, (unsigned long long)size);

  char *buf = malloc(DAV_SCRATCH + DAV_MAX_PATH);
  if (!buf) {
    return ESP_ERR_NO_MEM;
  }

  int len;
  if (is_dir) {
    len = snprintf(buf, DAV_SCRATCH + DAV_MAX_PATH,
                   "<D:response>"
                   "<D:href>%s</D:href>"
                   "<D:propstat>"
                   "<D:prop>"
                   "<D:resourcetype><D:collection/></D:resourcetype>"
                   "<D:getlastmodified>%s</D:getlastmodified>"
                   "</D:prop>"
                   "<D:status>HTTP/1.1 200 OK</D:status>"
                   "</D:propstat>"
                   "</D:response>",
                   href_path, datebuf);
  } else {
    len = snprintf(buf, DAV_SCRATCH + DAV_MAX_PATH,
                   "<D:response>"
                   "<D:href>%s</D:href>"
                   "<D:propstat>"
                   "<D:prop>"
                   "<D:resourcetype/>"
                   "<D:getcontentlength>%ld</D:getcontentlength>"
                   "<D:getlastmodified>%s</D:getlastmodified>"
                   "<D:getetag>%s</D:getetag>"
                   "<D:getcontenttype>application/octet-stream</D:getcontenttype>"
                   "</D:prop>"
                   "<D:status>HTTP/1.1 200 OK</D:status>"
                   "</D:propstat>"
                   "</D:response>",
                   href_path, (long)size, datebuf, etag);
  }

  esp_err_t err = ESP_OK;
  if (len > 0) {
    err = httpd_resp_send_chunk(req, buf, len);
  }
  free(buf);
  return err;
}

static esp_err_t dav_propfind_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  char fs_path[DAV_MAX_PATH];
  if (!uri_to_fs(req->uri, fs_path, sizeof(fs_path))) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid path");
    return ESP_FAIL;
  }

  /* Depth: 0 means this resource only, 1 means it plus immediate children.
     A missing header means infinite; we cap that at 1 for now. */
  int depth = 1;
  size_t dlen = httpd_req_get_hdr_value_len(req, "Depth");
  if (dlen > 0 && dlen < 16) {
    char dbuf[16];
    if (httpd_req_get_hdr_value_str(req, "Depth", dbuf, sizeof(dbuf)) == ESP_OK) {
      if (strcmp(dbuf, "0") == 0) depth = 0;
    }
  }

  struct stat st;
  if (stat(fs_path, &st) != 0) {
    ESP_LOGW(TAG, "PROPFIND %s -> 404 (%s)", req->uri, fs_path);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
  }

  bool is_dir = S_ISDIR(st.st_mode);
  ESP_LOGI(TAG, "PROPFIND %s depth=%d (%s)", req->uri, depth,
           is_dir ? "collection" : "file");

  /* Normalised URI path for hrefs: always starts with '/', no trailing slash
     except for root. */
  char uri_path[DAV_MAX_PATH];
  {
    char raw[DAV_MAX_PATH];
    size_t n = 0;
    for (; req->uri[n] && req->uri[n] != '?' && n + 1 < sizeof(raw); n++) {
      raw[n] = req->uri[n];
    }
    raw[n] = '\0';
    uri_decode(uri_path, sizeof(uri_path), raw);
    size_t ul = strlen(uri_path);
    while (ul > 1 && uri_path[ul - 1] == '/') {
      uri_path[--ul] = '\0';
    }
    if (ul == 0) {
      strcpy(uri_path, "/");
    }
  }

  httpd_resp_set_status(req, "207 Multi-Status");
  httpd_resp_set_type(req, "application/xml; charset=utf-8");

  const char *head =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<D:multistatus xmlns:D=\"DAV:\">";
  if (httpd_resp_send_chunk(req, head, strlen(head)) != ESP_OK) {
    return ESP_FAIL;
  }

  /* The requested resource itself. */
  {
    char self_href[DAV_MAX_PATH];
    char escaped[DAV_MAX_PATH];
    char encoded[DAV_MAX_PATH];
    uri_encode(encoded, sizeof(encoded), uri_path);
    xml_escape(escaped, sizeof(escaped), encoded);
    str_copy(self_href, sizeof(self_href), escaped);
    if (is_dir) {
      append_slash(self_href, sizeof(self_href));
    }
    send_response_block(req, self_href, is_dir, st.st_size, st.st_mtime);
  }

  /* Immediate children, when this is a collection and depth allows. */
  if (is_dir && depth > 0) {
    DIR *dir = opendir(fs_path);
    if (dir) {
      struct dirent *ent;
      while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
          continue;
        }
        if (is_hidden_junk(ent->d_name)) {
          continue;
        }

        char child_fs[DAV_MAX_PATH];
        path_join(child_fs, sizeof(child_fs), fs_path, ent->d_name);

        struct stat cst;
        if (stat(child_fs, &cst) != 0) {
          continue;
        }
        bool child_dir = S_ISDIR(cst.st_mode);

        char child_uri[DAV_MAX_PATH];
        path_join(child_uri, sizeof(child_uri), uri_path, ent->d_name);

        char encoded[DAV_MAX_PATH];
        char escaped[DAV_MAX_PATH];
        char href[DAV_MAX_PATH];
        uri_encode(encoded, sizeof(encoded), child_uri);
        xml_escape(escaped, sizeof(escaped), encoded);
        str_copy(href, sizeof(href), escaped);
        if (child_dir) {
          append_slash(href, sizeof(href));
        }

        send_response_block(req, href, child_dir, cst.st_size, cst.st_mtime);
      }
      closedir(dir);
    } else {
      ESP_LOGW(TAG, "opendir failed for %s", fs_path);
    }
  }

  const char *tail = "</D:multistatus>";
  if (httpd_resp_send_chunk(req, tail, strlen(tail)) != ESP_OK) {
    return ESP_FAIL;
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

/* ---------------------------------------------------------------------------
   GET -- generalised from the original single-file handler
   --------------------------------------------------------------------------- */

static esp_err_t dav_get_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  char fs_path[DAV_MAX_PATH];
  if (!uri_to_fs(req->uri, fs_path, sizeof(fs_path))) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid path");
    return ESP_FAIL;
  }

  struct stat st;
  if (stat(fs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
    ESP_LOGW(TAG, "GET %s -> 404", req->uri);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  FILE *f = fopen(fs_path, "rb");
  if (!f) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "GET %s (%ld bytes)", req->uri, (long)st.st_size);

  /* The validator a client needs in order to write this file back safely. */
  char etag[48];
  compute_etag(&st, etag, sizeof(etag));
  httpd_resp_set_hdr(req, "ETag", etag);

  /* .md is served as text/plain so a browser shows it rather than downloading;
     everything else is opaque bytes. */
  const char *dot = strrchr(fs_path, '.');
  if (dot && strcasecmp(dot, ".md") == 0) {
    httpd_resp_set_type(req, "text/plain");
  } else {
    httpd_resp_set_type(req, "application/octet-stream");
  }

  char *buf = malloc(DAV_SCRATCH);
  if (!buf) {
    fclose(f);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }

  size_t n;
  while ((n = fread(buf, 1, DAV_SCRATCH, f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
      free(buf);
      fclose(f);
      return ESP_FAIL;
    }
  }
  free(buf);
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

/* ---------------------------------------------------------------------------
   GET /status -- free space

   Registered before the wildcard GET handler, which matters:
   httpd_find_uri_handler walks hd_calls[] in registration order and takes the
   first match (httpd_uri.c:94), so a wildcard registered first would swallow
   this.

   Consequence worth knowing: a real file named /status on the card becomes
   unreachable over GET, because this handler wins. It still appears in
   PROPFIND listings and can still be deleted or moved.
   --------------------------------------------------------------------------- */

/* Render a byte count as e.g. "14.2 GB". Integer maths throughout -- avoids
   pulling floating point into printf on a target where that is a config
   option rather than a given. */
static void human_bytes(uint64_t bytes, char *out, size_t out_len)
{
  const uint64_t KB = 1024ULL;
  const uint64_t MB = KB * 1024ULL;
  const uint64_t GB = MB * 1024ULL;

  if (bytes >= GB) {
    uint64_t tenths = (bytes * 10ULL) / GB;
    snprintf(out, out_len, "%llu.%llu GB", tenths / 10ULL, tenths % 10ULL);
  } else if (bytes >= MB) {
    uint64_t tenths = (bytes * 10ULL) / MB;
    snprintf(out, out_len, "%llu.%llu MB", tenths / 10ULL, tenths % 10ULL);
  } else if (bytes >= KB) {
    snprintf(out, out_len, "%llu KB", bytes / KB);
  } else {
    snprintf(out, out_len, "%llu bytes", bytes);
  }
}

static esp_err_t dav_status_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  uint64_t total = 0, freeb = 0;
  esp_err_t err = esp_vfs_fat_info(s_base_path, &total, &freeb);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_vfs_fat_info(%s) failed: %s", s_base_path, esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot stat filesystem");
    return ESP_FAIL;
  }

  uint64_t used = (total >= freeb) ? (total - freeb) : 0;

  /* Percentage without floating point: one decimal place. */
  uint64_t pct_tenths = total ? (used * 1000ULL) / total : 0;

  char h_total[32], h_used[32], h_free[32];
  human_bytes(total, h_total, sizeof(h_total));
  human_bytes(used,  h_used,  sizeof(h_used));
  human_bytes(freeb, h_free,  sizeof(h_free));

  ESP_LOGI(TAG, "status: %s free of %s", h_free, h_total);

  char body[512];
  int len = snprintf(body, sizeof(body),
                     "{\n"
                     "  \"mount\": \"%s\",\n"
                     "  \"total_bytes\": %llu,\n"
                     "  \"used_bytes\": %llu,\n"
                     "  \"free_bytes\": %llu,\n"
                     "  \"used_percent\": %llu.%llu,\n"
                     "  \"summary\": \"%s free of %s (%s used)\"\n"
                     "}\n",
                     s_base_path, total, used, freeb,
                     pct_tenths / 10ULL, pct_tenths % 10ULL,
                     h_free, h_total, h_used);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, len);
}

/* ---------------------------------------------------------------------------
   PUT
   --------------------------------------------------------------------------- */

static esp_err_t dav_put_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  char fs_path[DAV_MAX_PATH];
  if (!uri_to_fs(req->uri, fs_path, sizeof(fs_path))) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid path");
    return ESP_FAIL;
  }

  struct stat st;
  bool existed = (stat(fs_path, &st) == 0);
  if (existed && S_ISDIR(st.st_mode)) {
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
  }

  /* Evaluated before the file is opened: opening with "wb" truncates, so a
     failed precondition must be caught while the old contents still exist. */
  if (!preconditions_ok(req, existed, &st)) {
    ESP_LOGW(TAG, "PUT %s -> 412 precondition failed", req->uri);
    return ESP_OK;
  }

  FILE *f = fopen(fs_path, "wb");
  if (!f) {
    ESP_LOGE(TAG, "PUT %s -> cannot open for writing", fs_path);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
    return ESP_FAIL;
  }

  char *buf = malloc(DAV_SCRATCH);
  if (!buf) {
    fclose(f);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }

  int remaining = req->content_len;
  int total = 0;
  while (remaining > 0) {
    int want = remaining < DAV_SCRATCH ? remaining : DAV_SCRATCH;
    int got = httpd_req_recv(req, buf, want);
    if (got == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;  /* transient; retry the same chunk */
    }
    if (got <= 0) {
      ESP_LOGE(TAG, "PUT %s -> recv failed after %d bytes", req->uri, total);
      free(buf);
      fclose(f);
      unlink(fs_path);  /* don't leave a half-written file behind */
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
      return ESP_FAIL;
    }
    if (fwrite(buf, 1, got, f) != (size_t)got) {
      ESP_LOGE(TAG, "PUT %s -> write failed (card full?)", req->uri);
      free(buf);
      fclose(f);
      unlink(fs_path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
      return ESP_FAIL;
    }
    remaining -= got;
    total += got;
  }

  free(buf);
  fclose(f);
  ESP_LOGI(TAG, "PUT %s (%d bytes)", req->uri, total);

  /* Hand back the validator for the version just written, so a client can chain
     further edits without a round trip to re-read it. */
  struct stat post;
  if (stat(fs_path, &post) == 0) {
    char etag[48];
    compute_etag(&post, etag, sizeof(etag));
    httpd_resp_set_hdr(req, "ETag", etag);
  }

  /* 201 for a new resource, 204 when overwriting an existing one. */
  httpd_resp_set_status(req, existed ? "204 No Content" : "201 Created");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* ---------------------------------------------------------------------------
   DELETE
   --------------------------------------------------------------------------- */

/* Depth-first removal. Recursion is bounded by directory nesting on the card,
   and each frame holds one DAV_MAX_PATH buffer, so the handler runs on the
   enlarged 8 KB server stack. */
static bool remove_recursive(const char *path)
{
  struct stat st;
  if (stat(path, &st) != 0) {
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    return unlink(path) == 0;
  }

  DIR *dir = opendir(path);
  if (!dir) {
    return false;
  }
  bool ok = true;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    char child[DAV_MAX_PATH];
    path_join(child, sizeof(child), path, ent->d_name);
    if (!remove_recursive(child)) {
      ok = false;
    }
  }
  closedir(dir);

  return (rmdir(path) == 0) && ok;
}

static esp_err_t dav_delete_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  char fs_path[DAV_MAX_PATH];
  if (!uri_to_fs(req->uri, fs_path, sizeof(fs_path))) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid path");
    return ESP_FAIL;
  }

  /* Refuse to delete the mount root itself. */
  if (strcmp(fs_path, s_base_path) == 0) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
  }

  struct stat st;
  if (stat(fs_path, &st) != 0) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
  }

  if (!preconditions_ok(req, true, &st)) {
    ESP_LOGW(TAG, "DELETE %s -> 412 precondition failed", req->uri);
    return ESP_OK;
  }

  if (!remove_recursive(fs_path)) {
    ESP_LOGE(TAG, "DELETE %s -> failed", req->uri);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "DELETE %s", req->uri);
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* ---------------------------------------------------------------------------
   MKCOL
   --------------------------------------------------------------------------- */

static esp_err_t dav_mkcol_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  char fs_path[DAV_MAX_PATH];
  if (!uri_to_fs(req->uri, fs_path, sizeof(fs_path))) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid path");
    return ESP_FAIL;
  }

  struct stat st;
  if (stat(fs_path, &st) == 0) {
    /* RFC 4918: MKCOL on an existing resource is specifically 405, not a
       generic error. Clients rely on the distinction. */
    ESP_LOGW(TAG, "MKCOL %s -> 405 (already exists)", req->uri);
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
  }

  if (mkdir(fs_path, 0777) != 0) {
    ESP_LOGE(TAG, "MKCOL %s -> mkdir failed", req->uri);
    /* Missing intermediate collection is a 409 per the spec. */
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "MKCOL %s", req->uri);
  httpd_resp_set_status(req, "201 Created");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* ---------------------------------------------------------------------------
   MOVE
   --------------------------------------------------------------------------- */

/* The Destination header carries an absolute URL ("http://host/path") or an
   absolute path. Reduce either to the path portion. */
static bool destination_to_fs(const char *dest, char *out, size_t out_len)
{
  const char *p = dest;
  if (strncasecmp(p, "http://", 7) == 0) {
    p += 7;
  } else if (strncasecmp(p, "https://", 8) == 0) {
    p += 8;
  }
  if (p != dest) {
    const char *slash = strchr(p, '/');
    if (!slash) {
      return false;
    }
    p = slash;
  }
  if (*p != '/') {
    return false;
  }
  return uri_to_fs(p, out, out_len);
}

static esp_err_t dav_move_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  char src[DAV_MAX_PATH];
  if (!uri_to_fs(req->uri, src, sizeof(src))) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid path");
    return ESP_FAIL;
  }

  size_t dlen = httpd_req_get_hdr_value_len(req, "Destination");
  if (dlen == 0 || dlen >= DAV_MAX_PATH) {
    ESP_LOGW(TAG, "MOVE %s -> missing or oversized Destination header", req->uri);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
  }

  char dest_hdr[DAV_MAX_PATH];
  if (httpd_req_get_hdr_value_str(req, "Destination", dest_hdr, sizeof(dest_hdr)) != ESP_OK) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
  }

  char dst[DAV_MAX_PATH];
  if (!destination_to_fs(dest_hdr, dst, sizeof(dst))) {
    ESP_LOGW(TAG, "MOVE -> unparseable Destination '%s'", dest_hdr);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
  }

  struct stat st;
  if (stat(src, &st) != 0) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Source not found");
    return ESP_FAIL;
  }

  bool dest_existed = (stat(dst, &st) == 0);
  if (dest_existed) {
    /* FATFS rename() will not overwrite, so clear the destination first.
       Overwrite: T is the default when the header is absent. */
    char ovr[8];
    if (httpd_req_get_hdr_value_str(req, "Overwrite", ovr, sizeof(ovr)) == ESP_OK &&
        (ovr[0] == 'F' || ovr[0] == 'f')) {
      httpd_resp_set_status(req, "412 Precondition Failed");
      httpd_resp_send(req, NULL, 0);
      return ESP_FAIL;
    }
    if (!remove_recursive(dst)) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot replace destination");
      return ESP_FAIL;
    }
  }

  if (rename(src, dst) != 0) {
    ESP_LOGE(TAG, "MOVE %s -> %s failed", src, dst);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "MOVE %s -> %s", src, dst);
  httpd_resp_set_status(req, dest_existed ? "204 No Content" : "201 Created");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* Drain and discard a request body. Skipping this leaves unread bytes in the
   socket, which desynchronises the next request on a keep-alive connection. */
static bool discard_body(httpd_req_t *req)
{
  int remaining = req->content_len;
  if (remaining <= 0) {
    return true;
  }
  char sink[128];
  while (remaining > 0) {
    int want = remaining < (int)sizeof(sink) ? remaining : (int)sizeof(sink);
    int got = httpd_req_recv(req, sink, want);
    if (got == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (got <= 0) {
      return false;
    }
    remaining -= got;
  }
  return true;
}

/* ---------------------------------------------------------------------------
   HEAD

   esp_http_server does not derive HEAD from GET -- an unregistered HEAD is a
   405, which Windows Explorer issues during a file copy.
   --------------------------------------------------------------------------- */

static esp_err_t dav_head_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  char fs_path[DAV_MAX_PATH];
  if (!uri_to_fs(req->uri, fs_path, sizeof(fs_path))) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid path");
    return ESP_FAIL;
  }

  struct stat st;
  if (stat(fs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  char len[24];
  snprintf(len, sizeof(len), "%ld", (long)st.st_size);
  httpd_resp_set_hdr(req, "Content-Length", len);

  char etag[48];
  compute_etag(&st, etag, sizeof(etag));
  httpd_resp_set_hdr(req, "ETag", etag);

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_status(req, "200 OK");

  ESP_LOGI(TAG, "HEAD %s (%ld bytes)", req->uri, (long)st.st_size);

  /* Headers only, no body -- that is the whole point of HEAD. */
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* ---------------------------------------------------------------------------
   PROPPATCH

   Windows Explorer stamps a copied file's timestamps with PROPPATCH straight
   after the upload. A 405 here makes it treat the whole copy as failed and
   DELETE the file it just wrote -- the upload itself having already succeeded.

   This acknowledges the request without persisting the properties. The file
   contents are correct; only the client-supplied creation/modified metadata is
   dropped, and the card's own mtime from the write still stands.
   --------------------------------------------------------------------------- */

static esp_err_t dav_proppatch_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  char fs_path[DAV_MAX_PATH];
  if (!uri_to_fs(req->uri, fs_path, sizeof(fs_path))) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid path");
    return ESP_FAIL;
  }

  if (!discard_body(req)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
    return ESP_FAIL;
  }

  struct stat st;
  if (stat(fs_path, &st) != 0) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
  }

  char uri_path[DAV_MAX_PATH];
  {
    char raw[DAV_MAX_PATH];
    size_t n = 0;
    for (; req->uri[n] && req->uri[n] != '?' && n + 1 < sizeof(raw); n++) {
      raw[n] = req->uri[n];
    }
    raw[n] = '\0';
    char decoded[DAV_MAX_PATH];
    uri_decode(decoded, sizeof(decoded), raw);
    char encoded[DAV_MAX_PATH];
    uri_encode(encoded, sizeof(encoded), decoded);
    xml_escape(uri_path, sizeof(uri_path), encoded);
  }

  char *body = malloc(512 + DAV_MAX_PATH);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }
  int len = snprintf(body, 512 + DAV_MAX_PATH,
                     "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                     "<D:multistatus xmlns:D=\"DAV:\">"
                     "<D:response>"
                     "<D:href>%s</D:href>"
                     "<D:propstat>"
                     "<D:prop/>"
                     "<D:status>HTTP/1.1 200 OK</D:status>"
                     "</D:propstat>"
                     "</D:response>"
                     "</D:multistatus>",
                     uri_path);

  ESP_LOGI(TAG, "PROPPATCH %s (acknowledged, not persisted)", req->uri);

  httpd_resp_set_status(req, "207 Multi-Status");
  httpd_resp_set_type(req, "application/xml; charset=utf-8");
  esp_err_t err = httpd_resp_send(req, body, len);
  free(body);
  return err;
}

/* ---------------------------------------------------------------------------
   LOCK / UNLOCK

   Deliberately a stub. The Windows WebDAV redirector will not write file
   contents unless LOCK succeeds -- it PUTs a zero-byte file, issues LOCK, and
   aborts the write if that fails. It never verifies that the returned token
   corresponds to real server-side state, so handing back a well-formed
   synthetic token is enough to unblock it.

   What this does NOT do: track lock ownership, enforce locks on
   PUT/DELETE/MOVE, or expire them. Two clients writing the same file
   concurrently will still race. That is an accepted trade-off for a
   single-user pocket drive, not an oversight.
   --------------------------------------------------------------------------- */

static void make_lock_token(char *out, size_t out_len)
{
  /* UUID-shaped, which is what clients expect to see after
     "opaquelocktoken:". Uniqueness per request is all that matters here. */
  snprintf(out, out_len, "%08lx-%04lx-%04lx-%04lx-%08lx%04lx",
           (unsigned long)(esp_random()),
           (unsigned long)(esp_random() & 0xffff),
           (unsigned long)(esp_random() & 0xffff),
           (unsigned long)(esp_random() & 0xffff),
           (unsigned long)(esp_random()),
           (unsigned long)(esp_random() & 0xffff));
}

static esp_err_t dav_lock_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  char fs_path[DAV_MAX_PATH];
  if (!uri_to_fs(req->uri, fs_path, sizeof(fs_path))) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid path");
    return ESP_FAIL;
  }

  if (!discard_body(req)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
    return ESP_FAIL;
  }

  /* A LOCK on a path that does not exist creates a "lock-null" resource, which
     is how Windows reserves a filename before writing to it. Create the empty
     file so the subsequent PUT has somewhere to land. */
  struct stat st;
  bool existed = (stat(fs_path, &st) == 0);
  if (!existed) {
    FILE *f = fopen(fs_path, "wb");
    if (!f) {
      ESP_LOGE(TAG, "LOCK %s -> cannot create lock-null resource", fs_path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create");
      return ESP_FAIL;
    }
    fclose(f);
  }

  char token[48];
  make_lock_token(token, sizeof(token));

  char uri_path[DAV_MAX_PATH];
  {
    char raw[DAV_MAX_PATH];
    size_t n = 0;
    for (; req->uri[n] && req->uri[n] != '?' && n + 1 < sizeof(raw); n++) {
      raw[n] = req->uri[n];
    }
    raw[n] = '\0';
    char decoded[DAV_MAX_PATH];
    uri_decode(decoded, sizeof(decoded), raw);
    char encoded[DAV_MAX_PATH];
    uri_encode(encoded, sizeof(encoded), decoded);
    xml_escape(uri_path, sizeof(uri_path), encoded);
  }

  char hdr[80];
  snprintf(hdr, sizeof(hdr), "<opaquelocktoken:%s>", token);
  httpd_resp_set_hdr(req, "Lock-Token", hdr);

  char *body = malloc(1024 + DAV_MAX_PATH);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }
  int len = snprintf(body, 1024 + DAV_MAX_PATH,
                     "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                     "<D:prop xmlns:D=\"DAV:\">"
                     "<D:lockdiscovery>"
                     "<D:activelock>"
                     "<D:locktype><D:write/></D:locktype>"
                     "<D:lockscope><D:exclusive/></D:lockscope>"
                     "<D:depth>infinity</D:depth>"
                     "<D:timeout>Second-3600</D:timeout>"
                     "<D:locktoken><D:href>opaquelocktoken:%s</D:href></D:locktoken>"
                     "<D:lockroot><D:href>%s</D:href></D:lockroot>"
                     "</D:activelock>"
                     "</D:lockdiscovery>"
                     "</D:prop>",
                     token, uri_path);

  ESP_LOGI(TAG, "LOCK %s (%s)", req->uri, existed ? "existing" : "lock-null");

  /* 201 when the lock brought the resource into existence, 200 otherwise. */
  httpd_resp_set_status(req, existed ? "200 OK" : "201 Created");
  httpd_resp_set_type(req, "application/xml; charset=utf-8");
  esp_err_t err = httpd_resp_send(req, body, len);
  free(body);
  return err;
}

static esp_err_t dav_unlock_handler(httpd_req_t *req)
{
  DAV_REQUIRE_AUTH(req);

  if (!discard_body(req)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
    return ESP_FAIL;
  }

  /* Nothing to release, since nothing was ever held. */
  ESP_LOGI(TAG, "UNLOCK %s", req->uri);
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* ---------------------------------------------------------------------------
   Server startup
   --------------------------------------------------------------------------- */

/* Embedded by EMBED_TXTFILES in main/CMakeLists.txt. The _end symbols point one
   past the last byte; EMBED_TXTFILES appends a NUL, which mbedtls requires to be
   counted in the length for PEM material. */
extern const uint8_t server_crt_start[] asm("_binary_server_crt_start");
extern const uint8_t server_crt_end[]   asm("_binary_server_crt_end");
extern const uint8_t server_key_start[] asm("_binary_server_key_start");
extern const uint8_t server_key_end[]   asm("_binary_server_key_end");

httpd_handle_t webdav_start(const char *base_path)
{
  snprintf(s_base_path, sizeof(s_base_path), "%s", base_path);

  httpd_handle_t server = NULL;
  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();

  /* Verified against esp_https_server.h in this IDF version: httpd_ssl_config
     nests the plain httpd_config_t as `.httpd`, so the wildcard matcher and the
     handler limits go one level down, not at the top level. */
  config.httpd.uri_match_fn     = httpd_uri_match_wildcard;
  config.httpd.max_uri_handlers = 16;
  config.httpd.stack_size       = 10240;  /* TLS needs more than plain HTTP */
  config.httpd.lru_purge_enable = true;

  /* Field names differ from older IDF releases: the server certificate is
     `servercert` here, not `cacert_pem`. */
  config.servercert     = server_crt_start;
  config.servercert_len = server_crt_end - server_crt_start;
  config.prvtkey_pem    = server_key_start;
  config.prvtkey_len    = server_key_end - server_key_start;

  ESP_LOGI(TAG, "Starting WebDAV server (HTTPS) on port %d, root %s",
           config.port_secure, s_base_path);

  if (httpd_ssl_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTPS server");
    return NULL;
  }

  const httpd_uri_t handlers[] = {
      { .uri = "/*", .method = HTTP_OPTIONS,  .handler = dav_options_handler,  .user_ctx = NULL },
      { .uri = "/*", .method = HTTP_PROPFIND, .handler = dav_propfind_handler, .user_ctx = NULL },
      /* Must precede the wildcard GET handler below -- first match wins. */
      { .uri = "/status", .method = HTTP_GET, .handler = dav_status_handler,  .user_ctx = NULL },
      { .uri = "/*", .method = HTTP_GET,      .handler = dav_get_handler,      .user_ctx = NULL },
      { .uri = "/*", .method = HTTP_PUT,      .handler = dav_put_handler,      .user_ctx = NULL },
      { .uri = "/*", .method = HTTP_DELETE,   .handler = dav_delete_handler,   .user_ctx = NULL },
      { .uri = "/*", .method = HTTP_MKCOL,    .handler = dav_mkcol_handler,    .user_ctx = NULL },
      { .uri = "/*", .method = HTTP_MOVE,     .handler = dav_move_handler,     .user_ctx = NULL },
      { .uri = "/*", .method = HTTP_LOCK,      .handler = dav_lock_handler,      .user_ctx = NULL },
      { .uri = "/*", .method = HTTP_UNLOCK,    .handler = dav_unlock_handler,    .user_ctx = NULL },
      { .uri = "/*", .method = HTTP_HEAD,      .handler = dav_head_handler,      .user_ctx = NULL },
      { .uri = "/*", .method = HTTP_PROPPATCH, .handler = dav_proppatch_handler, .user_ctx = NULL },
  };

  for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
    esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to register handler %d: %s", (int)i, esp_err_to_name(err));
    }
  }

  return server;
}
