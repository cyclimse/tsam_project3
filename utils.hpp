#pragma once

#include <map>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace resistance {

enum struct Command {
  // Client or Server (Member)
  GET_MSG,
  SEND_MSG,
  // Client only
  LISTSERVERS,
  // Server only
  QUERYSERVERS,
  KEEPALIVE,
  LEAVE,
  STATUSREQ,
  CONNECTED,
};

enum struct Identity { MEMBER, CLIENT, SERVER };

static const std::map<std::string, std::pair<Command, Identity>> commands = {
    {"GET_MSG", {Command::GET_MSG, Identity::MEMBER}},
    {"SEND_MSG", {Command::SEND_MSG, Identity::MEMBER}},
    {"LISTSERVERS", {Command::LISTSERVERS, Identity::CLIENT}},
    {"QUERYSERVERS", {Command::QUERYSERVERS, Identity::SERVER}},
    {"KEEPALIVE", {Command::KEEPALIVE, Identity::SERVER}},
    {"LEAVE", {Command::LEAVE, Identity::SERVER}},
    {"STATUSREQ", {Command::STATUSREQ, Identity::SERVER}},
    {"CONNECTED", {Command::CONNECTED, Identity::SERVER}},
};

} // namespace resistance

std::string get_my_ip() {

  struct ifaddrs *myaddrs, *ifa;
  void *in_addr;
  char buf[64];

  bool next = false;

  if (getifaddrs(&myaddrs) != 0) {
    perror("getifaddrs");
    exit(1);
  }
  for (ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL)
      continue;
    if (!(ifa->ifa_flags & IFF_UP))
      continue;
    switch (ifa->ifa_addr->sa_family) {
    case AF_INET: {
      struct sockaddr_in *s4 = (struct sockaddr_in *)ifa->ifa_addr;
      in_addr = &s4->sin_addr;
      break;
    }
    case AF_INET6: {
      struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ifa->ifa_addr;
      in_addr = &s6->sin6_addr;
      break;
    }
    default:
      continue;
    }
    if (!inet_ntop(ifa->ifa_addr->sa_family, in_addr, buf, sizeof(buf))) {
      printf("%s: inet_ntop failed!\n", ifa->ifa_name);
    } else {
      // printf("%s: %s\n", ifa->ifa_name, buf);
      if (std::string{ifa->ifa_name} == "lo") {
        next = true;
      } else if (next) {
        return std::string{buf};
      }
    }
  }
  return std::string{buf};
}