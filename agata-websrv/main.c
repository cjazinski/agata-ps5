/* agata_ps5_websrv — minimal HTTP server payload for jailbroken PS5.
 *
 * Serves a small HTML page + a JSON status endpoint on port 6971.
 * Proof-of-concept skeleton for a Pegasus-DL-style homebrew web app,
 * built with ps5-payload-sdk, no external libraries.
 *
 * Deploy: nc -q0 <ps5-ip> 9021 < agata_ps5_websrv.elf
 * Then:   http://<ps5-ip>:6971/
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>


typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);


static const char PAGE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<title>Agata PS5</title>"
    "<style>body{background:#0e1116;color:#e6e6e6;font-family:system-ui;"
    "display:flex;align-items:center;justify-content:center;height:100vh;"
    "margin:0}div{text-align:center}h1{font-size:3rem;margin-bottom:.2em}"
    "p{color:#8b949e}</style></head>"
    "<body><div><h1>&#129302; Hello from the PS5</h1>"
    "<p>agata_ps5_websrv &middot; built with ps5-payload-sdk</p>"
    "<p><a style='color:#58a6ff' href='/status'>/status</a></p>"
    "</div></body></html>";

static const char STATUS[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"app\":\"agata_ps5_websrv\",\"status\":\"running\",\"platform\":\"PS5\","
    "\"sdk\":\"ps5-payload-sdk\"}";

static const char NOTFOUND[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n"
    "not found";


static void
notify(const char* msg) {
  notify_request_t req;
  bzero(&req, sizeof req);
  strncpy(req.message, msg, sizeof req.message - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}


int
main() {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if(s < 0) {
    notify("agata_ps5_websrv: socket failed");
    return 1;
  }

  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  struct sockaddr_in addr;
  bzero(&addr, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(6971);

  if(bind(s, (struct sockaddr*)&addr, sizeof addr) < 0) {
    notify("agata_ps5_websrv: bind failed");
    return 1;
  }
  if(listen(s, 8) < 0) {
    notify("agata_ps5_websrv: listen failed");
    return 1;
  }

  notify("agata_ps5_websrv running on port 6971");

  for(;;) {
    int c = accept(s, NULL, NULL);
    if(c < 0) {
      continue;
    }

    char buf[2048];
    ssize_t n = read(c, buf, sizeof buf - 1);
    if(n <= 0) {
      close(c);
      continue;
    }
    buf[n] = '\0';

    const char* resp;
    size_t len;
    if(strncmp(buf, "GET /status", 11) == 0) {
      resp = STATUS;
      len = sizeof STATUS - 1;
    } else if(strncmp(buf, "GET / ", 6) == 0 ||
              strncmp(buf, "GET /index.html", 15) == 0) {
      resp = PAGE;
      len = sizeof PAGE - 1;
    } else {
      resp = NOTFOUND;
      len = sizeof NOTFOUND - 1;
    }

    ssize_t off = 0;
    while(off < (ssize_t)len) {
      ssize_t w = write(c, resp + off, len - off);
      if(w <= 0) {
        break;
      }
      off += w;
    }

    close(c);
  }

  return 0;
}
