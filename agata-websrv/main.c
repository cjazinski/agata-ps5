/* agata_ps5_websrv v0.2 — Pegasus-style web app payload for jailbroken PS5.
 *
 * - Embedded SPA (ui.html embedded at build time via ui.h)
 * - /api/fs/list, /api/fs/download   — browse + fetch console files
 * - /api/fetch (POST)                — queue a download to console storage
 * - /api/jobs                        — download queue status (JSON)
 * - /status                          — payload status JSON
 *
 * Single-threaded accept loop; one request at a time. Downloads run in a
 * worker thread. Deploy: nc -q0 <ps5-ip> 9021 < agata_ps5_websrv.elf
 */

#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <time.h>

#include "ui.h"      /* embedded SPA (generated from ui.html) */
#include "http.h"    /* tiny http client + url parser */

#define PORT        6971
#define MAX_JOBS    64
#define RBUF        16384

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
typedef int (*notify_fn)(int, notify_request_t*, size_t, int);
static notify_fn g_notify = 0;

static void notify(const char* msg) {
  if(!g_notify) return;
  notify_request_t req;
  bzero(&req, sizeof req);
  strncpy(req.message, msg, sizeof req.message - 1);
  g_notify(0, &req, sizeof req, 0);
}

/* ---------------- download jobs ---------------- */

typedef enum { J_QUEUED, J_RUNNING, J_DONE, J_ERROR } jstate;

typedef struct job {
  int id;
  jstate state;
  char url[1024];
  char dest[512];
  char fname[256];
  unsigned long bytes;
  unsigned long total;
  char error[128];
} job_t;

static job_t g_jobs[MAX_JOBS];
static int g_job_count = 0;
static int g_next_id = 1;
static pthread_mutex_t g_jobs_mtx = PTHREAD_MUTEX_INITIALIZER;

static int job_add(const char* url, const char* dest) {
  pthread_mutex_lock(&g_jobs_mtx);
  if(g_job_count >= MAX_JOBS) { pthread_mutex_unlock(&g_jobs_mtx); return -1; }
  job_t* j = &g_jobs[g_job_count++];
  j->id = g_next_id++;
  j->state = J_QUEUED;
  strncpy(j->url, url, sizeof j->url - 1);
  strncpy(j->dest, dest, sizeof j->dest - 1);
  /* filename from URL path */
  const char* slash = strrchr(url, '/');
  const char* q = strchr(slash ? slash : url, '?');
  size_t n = q ? (size_t)(q - (slash ? slash : url)) : strlen(slash ? slash : url);
  if(n == 0 || n >= sizeof j->fname) n = sizeof j->fname - 1;
  strncpy(j->fname, slash ? slash + 1 : url, n);
  j->fname[n] = '\0';
  if(!j->fname[0]) strcpy(j->fname, "download.bin");
  j->bytes = j->total = 0;
  j->error[0] = '\0';
  int id = j->id;
  pthread_mutex_unlock(&g_jobs_mtx);
  return id;
}

/* worker thread: pops the oldest queued job */
static void* job_worker(void* arg) {
  (void)arg;
  for(;;) {
    job_t* j = 0;
    pthread_mutex_lock(&g_jobs_mtx);
    for(int i = 0; i < g_job_count; i++) {
      if(g_jobs[i].state == J_QUEUED) { j = &g_jobs[i]; j->state = J_RUNNING; break; }
    }
    pthread_mutex_unlock(&g_jobs_mtx);
    if(!j) { sleep(1); continue; }

    char path[768];
    snprintf(path, sizeof path, "%s/%s", j->dest, j->fname);
    if(http_download_to(j->url, path, &j->bytes, &j->total, j->error, sizeof j->error) == 0) {
      pthread_mutex_lock(&g_jobs_mtx); j->state = J_DONE; pthread_mutex_unlock(&g_jobs_mtx);
      char msg[120]; snprintf(msg, sizeof msg, "Agata: %s finished", j->fname);
      notify(msg);
    } else {
      pthread_mutex_lock(&g_jobs_mtx);
      if(j->state != J_ERROR) j->state = J_ERROR; /* canceled mid-download also lands here */
      pthread_mutex_unlock(&g_jobs_mtx);
    }
  }
  return 0;
}

/* ---------------- JSON + http helpers ---------------- */

static void jstr(char* out, size_t cap, const char* s) {
  /* JSON-escape s into out */
  size_t o = 0;
  out[o++] = '"';
  for(; *s && o + 8 < cap; s++) {
    if(*s == '"' || *s == '\\') { out[o++] = '\\'; out[o++] = *s; }
    else if((unsigned char)*s < 0x20) { o += snprintf(out + o, cap - o, "\\u%04x", *s); }
    else out[o++] = *s;
  }
  out[o++] = '"';
  out[o] = '\0';
}

/* ---------------- request handlers ---------------- */

static void resp_raw(int c, const char* code, const char* ctype, const char* body, size_t len) {
  char hdr[256];
  int hn = snprintf(hdr, sizeof hdr,
      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
      code, ctype, len);
  write(c, hdr, hn);
  size_t off = 0;
  while(off < len) {
    ssize_t w = write(c, body + off, len - off);
    if(w <= 0) break;
    off += w;
  }
}

static void resp_json(int c, int code, const char* json) {
  resp_raw(c, code == 200 ? "200 OK" : "400 Bad Request", "application/json", json, strlen(json));
}

/* GET /api/fs/list?path=... */
static void h_fs_list(int c, const char* path) {
  DIR* d = opendir(path);
  if(!d) {
    char b[256]; char p[192]; jstr(p, sizeof p, path);
    snprintf(b, sizeof b, "{\"error\":\"cannot open %s: %s\"}", p, strerror(errno));
    resp_json(c, 400, b);
    return;
  }
  char* buf = malloc(131072);
  size_t used = 0;
  used += snprintf(buf + used, 131072 - used, "{\"path\":");
  { char p[192]; jstr(p, sizeof p, path); used -= used; } /* placeholder, path echoed below */
  used = 0;
  used += snprintf(buf + used, 131072 - used, "{\"entries\":[");
  struct dirent* e;
  int first = 1;
  while((e = readdir(d)) != 0) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char full[1024];
    snprintf(full, sizeof full, "%s%s%s", path, path[strlen(path)-1] == '/' ? "" : "/", e->d_name);
    struct stat st;
    int is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    char name_esc[512]; jstr(name_esc, sizeof name_esc, e->d_name);
    int n = snprintf(buf + used, 131072 - used,
      "%s{\"name\":%s,\"is_dir\":%s,\"size\":%ld}",
      first ? "" : ",", name_esc, is_dir ? "true" : "false",
      (long)(!is_dir && stat(full, &st) == 0 ? st.st_size : 0));
    if(n < 0 || (size_t)n >= 131072 - used) break;
    used += n;
    first = 0;
  }
  closedir(d);
  used += snprintf(buf + used, 131072 - used, "]}");
  resp_raw(c, "200 OK", "application/json", buf, used);
  free(buf);
}

/* GET /api/fs/download?path=... */
static void h_fs_download(int c, const char* path) {
  FILE* f = fopen(path, "rb");
  if(!f) {
    resp_json(c, 400, "{\"error\":\"cannot open file\"}");
    return;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char hdr[256];
  const char* base = strrchr(path, '/');
  base = base ? base + 1 : path;
  int hn = snprintf(hdr, sizeof hdr,
    "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
    "Content-Length: %ld\r\nContent-Disposition: attachment; filename=\"%s\"\r\n"
    "Connection: close\r\n\r\n", sz, base);
  write(c, hdr, hn);
  char* buf = malloc(RBUF);
  size_t n;
  while((n = fread(buf, 1, RBUF, f)) > 0) {
    ssize_t off = 0;
    while(off < (ssize_t)n) {
      ssize_t w = write(c, buf + off, n - off);
      if(w <= 0) goto done;
      off += w;
    }
  }
done:
  free(buf);
  fclose(f);
}

/* POST /api/fetch  {url, dest} */
static void h_fetch(int c, const char* body) {
  char url[1024] = "", dest[512] = "";
  /* crude but safe JSON field extraction */
  const char* u = strstr(body, "\"url\"");
  const char* d = strstr(body, "\"dest\"");
  if(u) { u = strchr(u + 5, '"'); if(u) { u++; const char* e = strchr(u, '"'); if(e && e - u < 1024) { memcpy(url, u, e - u); url[e - u] = 0; } } }
  if(d) { d = strchr(d + 6, '"'); if(d) { d++; const char* e = strchr(d, '"'); if(e && e - d < 512) { memcpy(dest, d, e - d); dest[e - d] = 0; } } }
  if(!url[0]) { resp_json(c, 400, "{\"error\":\"url required\"}"); return; }
  if(!dest[0]) strcpy(dest, "/mnt/usb0");
  int id = job_add(url, dest);
  if(id < 0) { resp_json(c, 400, "{\"error\":\"queue full\"}"); return; }
  char b[128];
  snprintf(b, sizeof b, "{\"ok\":true,\"id\":%d}", id);
  resp_json(c, 200, b);
}

/* GET /api/jobs */
static void h_jobs(int c) {
  pthread_mutex_lock(&g_jobs_mtx);
  char* buf = malloc(65536);
  size_t used = snprintf(buf, 65536, "{\"jobs\":[");
  for(int i = 0; i < g_job_count; i++) {
    job_t* j = &g_jobs[i];
    char u[1200], dst[700], fn[600], er[300];
    jstr(u, sizeof u, j->url);
    jstr(dst, sizeof dst, j->dest);
    jstr(fn, sizeof fn, j->fname);
    jstr(er, sizeof er, j->error);
    const char* st = j->state == J_QUEUED ? "queued" : j->state == J_RUNNING ? "running"
                   : j->state == J_DONE ? "done" : "error";
    int n = snprintf(buf + used, 65536 - used,
      "%s{\"id\":%d,\"state\":\"%s\",\"url\":%s,\"dest\":%s,\"file\":%s,"
      "\"bytes\":%lu,\"total\":%lu,\"error\":%s}",
      i ? "," : "", j->id, st, u, dst, fn, j->bytes, j->total, er);
    if(n < 0 || (size_t)n >= 65536 - used) break;
    used += n;
  }
  used += snprintf(buf + used, 65536 - used, "]}");
  pthread_mutex_unlock(&g_jobs_mtx);
  resp_raw(c, "200 OK", "application/json", buf, used);
  free(buf);
}

/* ---- fs operations (move/copy/mkdir/delete) ---- */

static void json_str_get(const char* body, const char* key, char* out, size_t cap) {
  out[0] = '\0';
  char pat[32];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char* p = strstr(body, pat);
  if(!p) return;
  p = strchr(p + strlen(pat), '"');
  if(!p) return;
  p++;
  const char* e = strchr(p, '"');
  if(!e) return;
  size_t n = e - p;
  if(n >= cap) n = cap - 1;
  memcpy(out, p, n);
  out[n] = '\0';
}

static int copy_file(const char* src, const char* dst) {
  FILE* in = fopen(src, "rb");
  if(!in) return -1;
  FILE* out = fopen(dst, "wb");
  if(!out) { fclose(in); return -1; }
  char* buf = malloc(65536);
  size_t n;
  while((n = fread(buf, 1, 65536, in)) > 0) {
    if(fwrite(buf, 1, n, out) != n) { free(buf); fclose(in); fclose(out); return -1; }
  }
  free(buf);
  fclose(in); fclose(out);
  return 0;
}

static int rm_rf(const char* path, int depth) {
  if(depth > 16) return -1;
  struct stat st;
  if(stat(path, &st) != 0) return -1;
  if(!S_ISDIR(st.st_mode)) return unlink(path);
  DIR* d = opendir(path);
  if(!d) return -1;
  struct dirent* e;
  int rc = 0;
  while((e = readdir(d)) != 0) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char full[1024];
    snprintf(full, sizeof full, "%s%s%s", path, path[strlen(path)-1]=='/' ? "" : "/", e->d_name);
    if(rm_rf(full, depth + 1) != 0) rc = -1;
  }
  closedir(d);
  if(rmdir(path) != 0) rc = -1;
  return rc;
}

/* POST /api/fs/move|copy {src, dst_dir} ; POST /api/fs/mkdir {path} ; POST /api/fs/delete {path} */
static void h_fs_op(int c, const char* op, const char* body) {
  char src[1024], dst[1024], path[1024];
  char final[2048];
  if(strcmp(op, "mkdir") == 0) {
    json_str_get(body, "path", path, sizeof path);
    if(!path[0]) { resp_json(c, 400, "{\"error\":\"path required\"}"); return; }
    if(mkdir(path, 0777) == 0) resp_json(c, 200, "{\"ok\":true}");
    else { char b[256]; snprintf(b, sizeof b, "{\"error\":\"mkdir: %s\"}", strerror(errno)); resp_json(c, 400, b); }
    return;
  }
  if(strcmp(op, "delete") == 0) {
    json_str_get(body, "path", path, sizeof path);
    if(!path[0]) { resp_json(c, 400, "{\"error\":\"path required\"}"); return; }
    if(rm_rf(path, 0) == 0) resp_json(c, 200, "{\"ok\":true}");
    else { char b[256]; snprintf(b, sizeof b, "{\"error\":\"delete: %s\"}", strerror(errno)); resp_json(c, 400, b); }
    return;
  }
  /* move / copy */
  json_str_get(body, "src", src, sizeof src);
  json_str_get(body, "dst_dir", dst, sizeof dst);
  if(!src[0] || !dst[0]) { resp_json(c, 400, "{\"error\":\"src and dst_dir required\"}"); return; }
  const char* base = strrchr(src, '/');
  base = base ? base + 1 : src;
  snprintf(final, sizeof final, "%s%s%s", dst, dst[strlen(dst)-1]=='/' ? "" : "/", base);
  int rc;
  if(strcmp(op, "move") == 0) rc = rename(src, final);
  else rc = copy_file(src, final);
  if(rc == 0) { char f[1200]; jstr(f, sizeof f, final); char b[1400]; snprintf(b, sizeof b, "{\"ok\":true,\"path\":%s}", f); resp_json(c, 200, b); }
  else { char b[256]; snprintf(b, sizeof b, "{\"error\":\"%s: %s\"}", op, strerror(errno)); resp_json(c, 400, b); }
}

static const char STATUS_JSON[] =
  "{\"app\":\"agata_ps5_websrv\",\"version\":\"0.2.0\",\"status\":\"running\","
  "\"platform\":\"PS5\",\"sdk\":\"ps5-payload-sdk\",\"endpoints\":"
  "[\"/\",\"/status\",\"/api/fs/list\",\"/api/fs/download\",\"/api/fetch\",\"/api/jobs\"]}";

/* ---------------- main server ---------------- */

static void urldecode(char* s) {
  char* o = s;
  for(; *s; s++) {
    if(*s == '%' && s[1] && s[2]) {
      int hi = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
      int lo = s[2] <= '9' ? s[2] - '0' : (s[2] | 32) - 'a' + 10;
      *o++ = hi * 16 + lo;
      s += 2;
    } else if(*s == '+') *o++ = ' ';
    else *o++ = *s;
  }
  *o = '\0';
}

/* extract ?param= value into out (raw, then urldecoded) */
static int get_param(const char* req, const char* name, char* out, size_t cap) {
  char pat[64];
  snprintf(pat, sizeof pat, "%s=", name);
  const char* p = strstr(req, pat);
  if(!p) return -1;
  p += strlen(pat);
  const char* e = p;
  while(*e && *e != ' ' && *e != '&' && *e != '\r' && *e != '\n') e++;
  size_t n = e - p;
  if(n >= cap) n = cap - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  urldecode(out);
  return 0;
}

int main() {
  g_notify = (notify_fn)dlsym(RTLD_DEFAULT, "sceKernelSendNotificationRequest");

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if(s < 0) { notify("agata: socket failed"); for(;;) pause(); }
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  struct sockaddr_in addr;
  bzero(&addr, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(PORT);
  if(bind(s, (struct sockaddr*)&addr, sizeof addr) < 0) { notify("agata: bind failed"); for(;;) pause(); }
  if(listen(s, 16) < 0) { notify("agata: listen failed"); for(;;) pause(); }

  pthread_t th;
  pthread_create(&th, 0, job_worker, 0);

  notify("agata_ps5_websrv v0.2 on port 6971");

  for(;;) {
    int c = accept(s, NULL, NULL);
    if(c < 0) continue;
    char buf[8192];
    ssize_t n = read(c, buf, sizeof buf - 1);
    if(n <= 0) { close(c); continue; }
    buf[n] = '\0';

    char path[1024] = "";
    {
      /* first line: METHOD /path HTTP/x */
      char* sp = strchr(buf, ' ');
      if(sp) {
        sp++;
        char* e = strchr(sp, ' ');
        size_t pl = e ? (size_t)(e - sp) : 0;
        if(pl >= sizeof path) pl = sizeof path - 1;
        memcpy(path, sp, pl);
        path[pl] = '\0';
      }
    }

    char param[1024];

    if(strncmp(path, "/api/fs/list", 12) == 0) {
      if(get_param(buf, "path", param, sizeof param) == 0) h_fs_list(c, param);
      else h_fs_list(c, "/mnt");
    } else if(strncmp(path, "/api/fs/download", 16) == 0) {
      if(get_param(buf, "path", param, sizeof param) == 0) h_fs_download(c, param);
      else resp_json(c, 400, "{\"error\":\"path required\"}");
    } else if(strncmp(path, "/api/fs/move", 12) == 0 ||
              strncmp(path, "/api/fs/copy", 12) == 0 ||
              strncmp(path, "/api/fs/mkdir", 13) == 0 ||
              strncmp(path, "/api/fs/delete", 14) == 0) {
      char* body = strstr(buf, "\r\n\r\n");
      const char* op = strncmp(path, "/api/fs/move", 12) == 0 ? "move"
                     : strncmp(path, "/api/fs/copy", 12) == 0 ? "copy"
                     : strncmp(path, "/api/fs/mkdir", 13) == 0 ? "mkdir" : "delete";
      if(body) h_fs_op(c, op, body + 4);
      else resp_json(c, 400, "{\"error\":\"body required\"}");
    } else if(strncmp(path, "/api/jobs", 9) == 0) {
      h_jobs(c);
    } else if(strncmp(path, "/api/fetch", 10) == 0) {
      /* body starts after \r\n\r\n */
      char* body = strstr(buf, "\r\n\r\n");
      if(body) h_fetch(c, body + 4);
      else resp_json(c, 400, "{\"error\":\"body required\"}");
    } else if(strcmp(path, "/status") == 0) {
      resp_json(c, 200, STATUS_JSON);
    } else if(strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
      resp_raw(c, "200 OK", "text/html; charset=utf-8", UI_HTML, UI_HTML_LEN);
    } else {
      resp_raw(c, "404 Not Found", "text/plain", "not found", 9);
    }

    close(c);
  }
  return 0;
}
