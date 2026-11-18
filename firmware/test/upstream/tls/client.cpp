/* test_up_tls: opens bell::TLSSocket (cspot's TLS, as patched) to HOST:PORT
 * and says whether the handshake went through. Exit 0: connected (with
 * "greet", the server's greeting read too); 1: the socket refused the
 * server (threw). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdexcept>

#include "BellLogger.h"
#include "TLSSocket.h"

int main(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    fprintf(stderr, "usage: client HOST PORT [greet]\n");
    return 2;
  }
  bell::setDefaultLogger();
  try {
    bell::TLSSocket s;
    s.open(argv[1], (uint16_t) atoi(argv[2]));
    printf("connected\n");
    if (argc == 4) {
      uint8_t buf[16] = {0};
      int n = (int) s.read(buf, sizeof(buf) - 1);
      printf("read %d: %s\n", n, n > 0 ? (char *) buf : "");
    }
    s.close();
    return 0;
  } catch (const std::exception &e) {
    printf("refused: %s\n", e.what());
    return 1;
  }
}
