#pragma once

#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h> /* close */

namespace recipient {
typedef std::pair<std::string, std::string> Recipient;

static constexpr struct addrinfo hints_for_listen {
  AI_PASSIVE, AF_INET, SOCK_STREAM, IPPROTO_TCP
};

static constexpr struct addrinfo hints_for_connect {
  0, AF_INET, SOCK_STREAM, IPPROTO_TCP
};

int get_sock(bool isForListen, std::string const node = nullptr,
             std::string const service = nullptr) {

  struct addrinfo *addresses;
  int s;

  if (isForListen) {
    s = getaddrinfo(nullptr, service.c_str(), &hints_for_listen, &addresses);
  } else {
    s = getaddrinfo(node.c_str(), service.c_str(), &hints_for_connect,
                    &addresses);
  }

  if (s != 0) {
    std::cerr << gai_strerror(s) << std::endl;
    throw std::runtime_error("Could not create recipient");
  }

  struct addrinfo *rp;

  int sock;

  for (rp = addresses; rp != nullptr; rp = rp->ai_next) {

    if (isForListen) {
      sock = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK,
                    rp->ai_protocol);
    } else {
      sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    }

    if (sock == -1) {
      continue;
    }

    if (isForListen) {
      int set = 1;
      if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0) {
        continue;
      }

      if (bind(sock, rp->ai_addr, rp->ai_addrlen) != -1) {
        break; /* Success */
      }
    } else {
      if (connect(sock, rp->ai_addr, rp->ai_addrlen) != -1)
        break; /* Success */
    }

    close(sock);
  }

  freeaddrinfo(addresses);

  if (rp == nullptr) {
    throw std::runtime_error("Failed to create a socket");
  } else {
    return sock;
  }
};

} // namespace recipient
