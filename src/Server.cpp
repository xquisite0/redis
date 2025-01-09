#include "ProtocolParser.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
#include <utility>

void handleClient(int client_fd) {
  std::unordered_map<std::string, std::string> keyValue;
  std::unordered_map<std::string, std::pair<time_t, long>> keyStartExpiry;

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
              time_t set_time;
              time(&set_time);

              keyStartExpiry[message.elements[1].value] =
                  std::make_pair(set_time, stol(message.elements[4].value));
            }
          }

        } else if (command == "get") {
          // key has not been set
          if (keyValue.find(message.elements[1].value) == keyValue.end()) {
            response = "$-1\r\n";
            break;
          }

          // key has expired
          if (keyStartExpiry.find(message.elements[1].value) !=
              keyStartExpiry.end()) {
            time_t get_time;
            time(&get_time);

            time_t set_time = keyStartExpiry[message.elements[1].value].first;
            int expiry = keyStartExpiry[message.elements[1].value]
                             .second; // in milliseconds

            double duration = difftime(get_time, set_time); // in seconds
            std::cout << "\nDuration: " << duration << "\nExpiry: " << expiry
                      << "\n";
            if (duration * 1000 < expiry) {
              response = "$-1\r\n";
              break;
            }
          }

          std::string value = keyValue[message.elements[1].value];
          response =
              "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
        }
      }
    }

    // std::string response = "+PONG\r\n";
    send(client_fd, response.c_str(), response.size(), 0);
  }
  close(client_fd);
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

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
      std::thread(handleClient, client_fd).detach();
    }

    std::cout << "Client connected\n" << client_fd;
  }

  close(server_fd);

  return 0;
}
