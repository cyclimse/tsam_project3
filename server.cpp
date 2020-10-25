#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h> /* close */

#include "recipient.hpp"
#include "utils.hpp"

void dump(std::string s) {
  for (unsigned int n = 0; n < s.length(); ++n) {
    char c = s[n];
    std::cout << (int)c << ",";
  }
  std::cout << std::endl;
}

namespace resistance {

struct Member {
  Identity identity;
  std::string ip;
  int port;
  std::string group_id;
  int number_of_messages;
};

} // namespace resistance

namespace rs = resistance;

struct Server {

  Server(std::string const service) noexcept
      : service(service), my_ip(get_my_ip()) {}

  void run() {

    try {
      listenSock = recipient::get_sock(true, "", service);
    } catch (const std::runtime_error &e) {
      std::cerr << "Could not setup socket: " << e.what() << std::endl;
      exit(1);
    }

    if (listen(listenSock, backlog) < 0) {
      throw std::runtime_error("Failed to listen on socket: " + service);
    }

    FD_ZERO(&openSockets);
    FD_SET(listenSock, &openSockets);
    maxfds = listenSock;

    int clientSock;
    struct sockaddr_in client;
    socklen_t clientLen;

    size_t length;

    struct timespec start, finish;

    while (true) {

      readbuffer.fill(0);
      indexes_to_disconnect.erase(indexes_to_disconnect.begin(),
                                  indexes_to_disconnect.end());

      // Get modifiable copy of readSockets
      readSockets = exceptSockets = openSockets;

      while (!to_connect.empty()) {

        try {
          clientSock = recipient::get_sock(false, to_connect.front().first,
                                           to_connect.front().second);
        } catch (const std::runtime_error &e) {
          std::cerr << e.what() << std::endl;
          to_connect.pop();
          continue;
        }

        // Add new client to the list of open sockets
        FD_SET(clientSock, &openSockets);
        FD_SET(clientSock, &readSockets);

        // And update the maximum file descriptor
        maxfds = std::max(maxfds, clientSock);

        members[clientSock] =
            rs::Member{rs::Identity::SERVER, to_connect.front().first,
                       std::stoi(to_connect.front().second)};
        greet(clientSock);
        to_connect.pop();
      }

      // Send messages to connected servers to transmit data
      work(start, finish);

      clock_gettime(CLOCK_MONOTONIC, &start);

      // Look at sockets and see which ones have something to be read()
      int n =
          select(maxfds + 1, &readSockets, nullptr, &exceptSockets, nullptr);

      if (n < 0) {
        throw std::runtime_error("Select failed");
      } else if (FD_ISSET(listenSock, &readSockets)) {
        clientSock = accept(listenSock, (struct sockaddr *)&client, &clientLen);

        if (clientSock != -1) {

          // Add new client to the list of open sockets
          FD_SET(clientSock, &openSockets);

          // And update the maximum file descriptor
          maxfds = std::max(maxfds, clientSock);

          // create a new client to store information.
          char ip[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &(client.sin_addr), ip, INET_ADDRSTRLEN);
          members[clientSock] =
              rs::Member{rs::Identity::CLIENT, ip, ntohs(client.sin_port)};

          // Decrement the number of sockets waiting to be dealt with
          n--;

          printf("Client connected on server: %d\n", clientSock);
        }
      }

      while (n-- > 0) {
        for (auto &pair : members) {

          if (FD_ISSET(pair.first, &readSockets)) {

            length = recv(pair.first, readbuffer.data(), readbuffer_size,
                          MSG_DONTWAIT);

            // recv() == 0 means client has closed connection
            if (length == 0) {
              indexes_to_disconnect.push_back(pair.first);
              closeClient(pair.first);
            }

            // We don't check for -1 (nothing received) because select()
            // only triggers if there is something on the socket for us.
            else {

              parseCommand(pair.first, pair.second,
                           std::string{readbuffer.data(), length});
            }
          }
        }
      }

      // Remove client from the clients list
      for (int const index : indexes_to_disconnect) {
        members.erase(index);
      }
    }

    close(listenSock);
  }

  void parseCommand(int clientSocket, rs::Member &member,
                    std::string &&command) {

    // Remove all control characters
    command.erase(
        std::remove_if(command.begin(), command.end(),
                       [](unsigned char x) { return std::iscntrl(x); }),
        command.end());

    if (command.front() == '*' && command.back() == '#') {
      command = command.substr(1, command.size() - 2);
    }

    static const std::string color_cyan{"\033[0;36m"};
    static const std::string color_magenta{"\033[0;35m"};
    static const std::string end_color{"\033[0m"};
    if (member.identity == rs::Identity::MEMBER ||
        member.identity == rs::Identity::CLIENT) {
      std::cout << color_cyan << "CLIENT ON " << clientSocket << '\n';
      std::cout << "  IP: " << member.ip << '\n';
      std::cout << "  PORT: " << member.port << '\n';
    } else {
      std::cout << color_cyan << "SERVER ON " << clientSocket << '\n';
      std::cout << "  IP: " << member.ip << '\n';
      std::cout << "  PORT: " << member.port << '\n';
      std::cout << "  GROUP_ID: " << member.group_id << '\n';
    }

    std::cout << end_color << color_magenta << "  " << command << end_color
              << std::endl;

    std::istringstream ss(command);
    std::string token;
    std::getline(ss, token, ',');

    const auto it = rs::commands.find(token);

    if (it == rs::commands.end()) {
      std::string msg = "Unknown command";
      send(clientSocket, msg.c_str(), msg.length(), 0);
    };

    // We prevent clients from sending server-specific messages
    if (it->second.second == resistance::Identity::SERVER &&
        it->second.first != resistance::Command::QUERYSERVERS &&
        member.identity != resistance::Identity::SERVER) {
      std::string msg = "Trying to use server commands as client";
      send(clientSocket, msg.c_str(), msg.length(), 0);
      return;
    }

    switch (it->second.first) {
    case resistance::Command::GET_MSG: {
      std::getline(ss, token, ',');
      std::string msg;
      while (messages.find(token) != messages.end() &&
             !messages.at(token).empty()) {
        msg += "*SEND_MSG,";
        msg += token;
        msg += ',';
        msg += messages[token].front().first;
        msg += ',';
        msg += messages[token].front().second;
        msg += '#';
        messages[token].pop();
        send(clientSocket, msg.c_str(), msg.length(), 0);
        msg.erase(msg.begin(), msg.end());
      }
      break;
    }
    case resistance::Command::SEND_MSG:
      if (member.identity == resistance::Identity::CLIENT) {
        std::getline(ss, token, ',');
        std::string index = token;
        std::cout << "Index: " << index << std::endl;
        std::getline(ss, token, ',');
        messages[index].push(Message{my_group_id, token});
      } else {
        std::getline(ss, token, ',');
        std::string index = token;
        std::getline(ss, token, ',');
        std::string from = token;
        std::getline(ss, token, ',');
        messages[index].push(Message{from, token});
      }
      break;
    case resistance::Command::LISTSERVERS:
      if (member.identity == resistance::Identity::CLIENT) {
        std::string msg = listServers();
        // Not very efficient
        msg.insert(0, "*CONNECTED,");
        msg += '#';
        send(clientSocket, msg.c_str(), msg.length(), 0);
      }
      break;
    case resistance::Command::QUERYSERVERS: {
      std::getline(ss, token, ',');
      if (member.identity == resistance::Identity::CLIENT) {
        if (token == my_group_id) {
          throw std::runtime_error{"Trying to connect to myself!"};
        }
        member.identity = rs::Identity::SERVER;
        member.group_id = token;
      }
      std::string msg = listServers();
      // Not very efficient
      msg.insert(0, "*CONNECTED,");
      msg += '#';
      send(clientSocket, msg.c_str(), msg.length(), 0);
      break;
    }
    case resistance::Command::KEEPALIVE: {
      std::getline(ss, token, ',');
      try {
        member.number_of_messages = std::stoi(token);
      } catch (const std::invalid_argument &e) {
        std::cout << e.what() << std::endl;
      }
      break;
    }
    case resistance::Command::LEAVE: {
      std::getline(ss, token, ',');
      std::string ip = token;
      std::getline(ss, token, ',');
      if (ip == my_ip && token == service) {
        indexes_to_disconnect.push_back(clientSocket);
        closeClient(clientSocket);
      }
      break;
    }
    case resistance::Command::STATUSREQ: {
      std::string msg;
      msg += "*STATUSRESP,";
      for (auto const &p : messages) {
        msg += p.first;
        msg += ',';
        msg += p.second.size();
        msg += ',';
      }
      msg += '#';
      send(clientSocket, msg.c_str(), msg.length(), 0);
      break;
    }
    case resistance::Command::CONNECTED:
      std::getline(ss, token, ',');
      member.group_id = token;

      // Parasite mode, will lead to crashes 
      // std::string token_of_token;

      // while (std::getline(ss, token, ';')) {
      //   std::istringstream ss(token);
      //   std::getline(ss, token_of_token, ',');
      //   if (token_of_token != my_group_id) {
      //     std::getline(ss, token_of_token, ',');
      //     std::string ip = token_of_token;
      //     std::getline(ss, token_of_token, ',');
      //     requestConnection({ip, token_of_token});
      //   }
      // }
      
      break;
    }
  }

  std::string listServers() {
    std::string result;
    result += my_group_id;
    result += ',';
    result += my_ip + ',';
    result += service + ';';
    for (auto const &p : members) {
      if (p.second.identity == rs::Identity::SERVER) {
        result += p.second.group_id + ',';
        result += p.second.ip + ',';
        result += std::to_string(p.second.port);
        result += ';';
      }
    }
    return result;
  }

  void closeClient(int clientSocket) {

    // If this client's socket is maxfds then the next lowest
    // one has to be determined. Socket fd's can be reused by the Kernel,
    // so there aren't any nice ways to do this.

    close(clientSocket);

    if (maxfds == clientSocket) {
      for (auto const &p : members) {
        maxfds = std::max(maxfds, p.first);
      }
    }

    // And remove from the list of open sockets.
    FD_CLR(clientSocket, &openSockets);
  }

  void requestConnection(recipient::Recipient &&recipient) {
    bool already_connected = false;
    for (auto const &p : members) {
      if (p.second.identity == rs::Identity::SERVER) {
        if (p.second.ip == recipient.first &&
            p.second.port == std::stoi(recipient.second)) {
          already_connected = true;
        }
      }
    }
    if (!already_connected) {
      to_connect.push(recipient);
    }
  }

  inline void greet(int clientSock) {
    std::string greetings = "*QUERYSERVERS," + std::string{my_group_id} + "#";
    send(clientSock, greetings.c_str(), greetings.size(), 0);
  }

  void work(struct timespec &start, struct timespec &finish) {
    clock_gettime(CLOCK_MONOTONIC, &finish);
    double elapsed = (finish.tv_sec - start.tv_sec);

    std::cout << "Elapsed: " << elapsed << std::endl;

    for (auto &pair : members) {
      if (pair.second.identity == rs::Identity::SERVER) {
        std::string msg;
        while (messages.find(pair.second.group_id) != messages.end() &&
               !messages.at(pair.second.group_id).empty()) {
          msg += "*SEND_MSG,";
          msg += pair.second.group_id;
          msg += ',';
          msg += messages[pair.second.group_id].front().first;
          msg += ',';
          msg += messages[pair.second.group_id].front().second;
          msg += '#';
          messages[pair.second.group_id].pop();
          send(pair.first, msg.c_str(), msg.length(), 0);
          msg.erase(msg.begin(), msg.end());
        }
        if (pair.second.number_of_messages) {
          msg = "*GET_MSG," + std::string{my_group_id} + '#';
          send(pair.first, msg.c_str(), msg.length(), 0);
          msg.erase(msg.begin(), msg.end());
        }
        // if (elapsed > 60 && messages.find(server->group_id) !=
        // messages.end()) {
        //   msg += "*KEEPALIVE,";
        //   msg += messages.at(server->group_id).size();
        //   msg += '#';
        //   send(pair.first, msg.c_str(), msg.length(), 0);
        //   msg.erase(msg.begin(), msg.end());
        // }
      }
    }
  }

  std::string my_group_id = "P3_GROUP_120";

private:
  std::string const my_ip;
  std::string const service;
  int listenSock;

  std::map<int, rs::Member> members;
  std::vector<int> indexes_to_disconnect;

  static constexpr int readbuffer_size{5000};
  std::array<char, readbuffer_size> readbuffer;

  typedef std::pair<std::string, std::string> Message;
  std::unordered_map<std::string, std::queue<Message>> messages;

  // This is used to setup new connections
  std::queue<recipient::Recipient> to_connect;

  fd_set openSockets;   // Current open sockets
  fd_set readSockets;   // Socket list for select()
  fd_set exceptSockets; // Exception socket list
  int maxfds;           // Passed to select() as max fd in set

  static constexpr int backlog{5};
};

int main(int argc, char *const argv[]) {

  if (argc != 2) {
    printf("Usage: server <port>\n");
    printf("Ctrl-C to terminate\n");
    exit(0);
  }

  std::string port = argv[1];
  auto server = Server(port);
  server.requestConnection({"127.0.0.1", "5000"});

  try {
    server.run();
  } catch (const std::runtime_error &e) {
    std::cerr << e.what() << std::endl;
  }
}