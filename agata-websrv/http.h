/* http.h — minimal HTTP/1.1 client for the PS5 payload (header-only).
 * Plain http:// only (TLS is out of scope for v0.2; the SPA fetches
 * https catalogs browser-side and hands the payload direct links).
 */
#ifndef AGATA_HTTP_H
#define AGATA_HTTP_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

typedef struct urlparts {
  char host[256];
  int port;
  char path[768];
} urlparts_t;

static int parse_url(const char* url, urlparts_t* up) {
  const char* p = strstr(url, "://");
  if(!p) return -1;
  p += 3;
  const char* slash = strchr(p, '/');
  const char* colon = strchr(p, ':');
  size_t hlen;
  up->port = 80;
  if(colon && (!slash || colon < slash)) {
    hlen = colon - p;
    up->port = atoi(colon + 1);
  } else {
    hlen = slash ? (size_t)(slash - p) : strlen(p);
  }
  if(hlen >= sizeof up->host) return -1;
  memcpy(up->host, p, hlen);
  up->host[hlen] = '\0';
  snprintf(up->path, sizeof up->path, "/%s", slash ? slash + 1 : "");
  return 0;
}

/* resolve host (handles both numeric and DNS via getaddrinfo) */
static int tcp_connect(const char* host, int port) {
  char portstr[8];
  snprintf(portstr, sizeof portstr, "%d", port);
  struct addrinfo hints, *res = 0;
  bzero(&hints, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if(getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;
  int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if(s < 0) { freeaddrinfo(res); return -1; }
  if(connect(s, res->ai_addr, res->ai_addrlen) < 0) {
    close(s); freeaddrinfo(res); return -1;
  }
  freeaddrinfo(res);
  return s;
}

/* Download url -> dest_path. Updates *bytes as it goes (caller publishes).
 * Returns 0 on success. On error fills errbuf and returns -1. */
static int http_download_to(const char* url, const char* dest_path,
                            unsigned long* bytes, unsigned long* total,
                            char* errbuf, size_t errcap) {
  urlparts_t up;
  if(parse_url(url, &up) != 0) {
    snprintf(errbuf, errcap, "bad url");
    return -1;
  }
  /* follow up to 5 redirects */
  char cururl[1024];
  strncpy(cururl, url, sizeof cururl - 1);
  int s = -1;
  char hdr[2048];
  for(int hop = 0; hop < 5; hop++) {
    if(parse_url(cururl, &up) != 0) { snprintf(errbuf, errcap, "bad redirect"); return -1; }
    s = tcp_connect(up.host, up.port);
    if(s < 0) { snprintf(errbuf, errcap, "connect %s failed", up.host); return -1; }
    int hn = snprintf(hdr, sizeof hdr,
      "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: agata-ps5/0.2\r\n"
      "Accept: */*\r\nConnection: close\r\n\r\n", up.path, up.host);
    if(write(s, hdr, hn) != hn) { close(s); snprintf(errbuf, errcap, "send failed"); return -1; }

    /* read status line + headers */
    size_t got = 0;
    while(got < sizeof hdr - 1) {
      ssize_t r = read(s, hdr + got, sizeof hdr - 1 - got);
      if(r <= 0) break;
      got += r;
      hdr[got] = '\0';
      if(strstr(hdr, "\r\n\r\n")) break;
    }
    hdr[got] = '\0';
    int code = 0;
    char loc[1024] = "";
    if(sscanf(hdr, "HTTP/%*f %d", &code) != 1) { close(s); snprintf(errbuf, errcap, "bad response"); return -1; }
    char* lp = strstr(hdr, "Location:");
    if(code >= 300 && code < 400) {
      if(lp) {
        lp += 9;
        while(*lp == ' ') lp++;
        char* e = lp;
        while(*e && *e != '\r' && *e != '\n') e++;
        size_t n = e - lp; if(n >= sizeof loc) n = sizeof loc - 1;
        memcpy(loc, lp, n); loc[n] = '\0';
        close(s);
        if(strncmp(loc, "http", 4) == 0) strncpy(cururl, loc, sizeof cururl - 1);
        else snprintf(cururl, sizeof cururl, "http://%s%s", up.host, loc);
        continue;
      }
      close(s); snprintf(errbuf, errcap, "redirect without location"); return -1;
    }
    if(code != 200) { close(s); snprintf(errbuf, errcap, "HTTP %d", code); return -1; }

    /* content-length if present */
    char* cl = strstr(hdr, "Content-Length:");
    *total = cl ? strtoul(cl + 15, 0, 10) : 0;

    /* find body start within hdr buffer */
    char* body = strstr(hdr, "\r\n\r\n");
    size_t bodylen = 0;
    if(body) { body += 4; bodylen = got - (size_t)(body - hdr); }

    /* stream to file */
    char pathbuf[1024];
    strncpy(pathbuf, dest_path, sizeof pathbuf - 1);
    FILE* f = fopen(pathbuf, "wb");
    if(!f) { close(s); snprintf(errbuf, errcap, "cannot write %s", pathbuf); return -1; }
    *bytes = 0;
    char rbuf[16384];
    if(bodylen) {
      fwrite(body, 1, bodylen, f);
      *bytes += bodylen;
    }
    for(;;) {
      ssize_t r = read(s, rbuf, sizeof rbuf);
      if(r <= 0) break;
      fwrite(rbuf, 1, r, f);
      *bytes += r;
    }
    fclose(f);
    close(s);
    return 0;
  }
  snprintf(errbuf, errcap, "too many redirects");
  return -1;
}

#endif
