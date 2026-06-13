#include "net.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

// First non-loopback, non-link-local IPv4 address on an UP interface.
// On a typical home device this is the LAN address; with multiple NICs we
// arbitrarily pick whichever getifaddrs reports first.
void collect_ip(char *out, size_t out_sz) {
  if (out_sz == 0)
    return;
  out[0] = '\0';
  struct ifaddrs *ifaddr = NULL;
  if (getifaddrs(&ifaddr) != 0)
    return;
  for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr)
      continue;
    if (ifa->ifa_flags & IFF_LOOPBACK)
      continue;
    if (!(ifa->ifa_flags & IFF_UP))
      continue;
    if (ifa->ifa_addr->sa_family != AF_INET)
      continue;
    struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
    uint32_t a = ntohl(sa->sin_addr.s_addr);
    // 169.254.0.0/16 link-local (auto-assigned, no real connectivity).
    if ((a & 0xFFFF0000u) == 0xA9FE0000u)
      continue;
    if (inet_ntop(AF_INET, &sa->sin_addr, out, out_sz))
      break;
  }
  freeifaddrs(ifaddr);
}

