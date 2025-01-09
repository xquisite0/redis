#include "ProtocolParser.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

void handleClient(int client_fd, const std::string &dir,
                  const std::string &dbfilename) {
  std::unordered_map<std::string, std::string> keyValue;
  std::unordered_map<
      std::string,
      std::pair<std::chrono::time_point<std::chrono::high_resolution_clock>,
                long>>
      keyStartExpiry;

  char buffer[1024];
  while (true) {
    int bytesRead = read(client_fd, buffer, sizeof(buffer));
    if (bytesRead <= 0)
      break;
    // std::cout << "Client: " << buffer << std::endl;

    ProtocolParser parser;
    RedisMessage message = parser.parse(buffer);
    std::string response;

    // Checking for ECHO command
    if (!message.elements.empty()) {
      RedisMessage firstElement = message.elements[0];
      if (firstElement.type == BULK_STRING) {

        std::string command = "";
        for (char c : firstElement.value) {
          command += tolower(c);
        }
        std::cout << "Command Name: " << command << "\n";
        if (command == "echo") {
          response = "+" + message.elements[1].value + "\r\n";

        } else if (command == "ping") {
          response = "+PONG\r\n";

        } else if (command == "set") {
          keyValue[message.elements[1].value] = message.elements[2].value;
          response = "+OK\r\n";

          if (message.elements.size() > 2) {
            if (message.elements[3].value == "px") {
              auto set_time = std::chrono::high_resolution_clock::now();

              keyStartExpiry[message.elements[1].value] =
                  std::make_pair(set_time, stol(message.elements[4].value));
            }
          }

        } else if (command == "get") {
          bool valid = true;

          // key has not been set
          if (keyValue.find(message.elements[1].value) == keyValue.end()) {
            response = "$-1\r\n";
            valid = false;
          }
          // key has expired
          if (keyStartExpiry.find(message.elements[1].value) !=
              keyStartExpiry.end()) {
            auto get_time = std::chrono::high_resolution_clock::now();
            auto set_time = keyStartExpiry[message.elements[1].value].first;
            int expiry = keyStartExpiry[message.elements[1].value].second;

            // std::cout << "\nget_time - set_time: " << get_time << " "
            // << set_time << "\n";

            std::chrono::duration<double, std::milli> duration =
                get_time - set_time;

            // double duration = (get_time - set_time).count();
            // int duration = difftime(get_time, set_time); // in seconds
            std::cout << "\nDuration: " << duration.count()
                      << "\nExpiry: " << expiry << "\n";
            if (duration.count() > expiry) {
              response = "$-1\r\n";
              valid = false;
            }
          }

          if (valid) {
            std::string value = keyValue[message.elements[1].value];
            response =
                "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
          }
        } else if (command == "config") {
          // CONFIG GET
          std::cout << "\2nd Argument from client:"
                    << strcasecmp(message.elements[1].value.c_str(), "get")
                    << "\n";
          if (message.elements.size() >= 2 &&
              strcasecmp(message.elements[1].value.c_str(), "get")) {
            if (message.elements[2].value == "dir") {
              response = "*2\r\n$3\r\ndir\r\n$" + std::to_string(dir.size()) +
                         "\r\n" + dir + "\r\n";
            } else if (message.elements[2].value == "dbfilename") {
              response = "*2\r\n$10\r\ndbfilename\r\n$" +
                         std::to_string(dbfilename.size()) + "\r\n" +
                         dbfilename + "\r\n";
            }
          }
        }
      }
    }

    // std::string response = "+PONG\r\n";
    // std::cout << "\nResponse to send: " << response << "\n";
    send(client_fd, response.c_str(), response.size(), 0);
  }
  close(client_fd);
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  std::string dir = "";
  std::string dbfilename = "";

  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--dir") {
      dir = argv[i + 1];
    }
    if (std::string(argv[i]) == "--dbfilename") {
      dbfilename = argv[i + 1];
    }
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Failed to create server socket\n";
    return 1;
  }

  // Since the tester restarts your program quite often, setting SO_REUSEADDR
  // ensures that we don't run into 'Address already in use' errors
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    std::cerr << "setsockopt failed\n";
    return 1;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(6379);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) !=
      0) {
    std::cerr << "Failed to bind to port 6379\n";
    return 1;
  }

  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }

  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);
  std::cout << "Waiting for a client to connect...\n";

  // You can use print statements as follows for debugging, they'll be visible
  // when running tests.
  std::cout << "Logs from your program will appear here!\n";

  // Uncomment this block to pass the first stage

  // accept(server_fd, (struct sockaddr *)&client_addr,
  //        (socklen_t *)&client_addr_len);
  while (true) {
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr,
                           (socklen_t *)&client_addr_len);

    if (client_fd >= 0) {
      std::thread(handleClient, client_fd, dir, dbfilename).detach();
    }

    std::cout << "Client connected\n" << client_fd;
  }

  close(server_fd);

  return 0;
}
