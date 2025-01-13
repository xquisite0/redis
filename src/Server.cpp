#include "ProtocolParser.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
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

static void readBytes(std::ifstream &is, char *buffer, int length) {
  if (!is.read(buffer, length)) {
    throw std::runtime_error("Unexpected EOF when reading RDB file");
  }
}

static uint8_t readByte(std::ifstream &is) {
  char byte;
  is.read(&byte, 1);
  return static_cast<uint8_t>(byte);
}

int readLength(std::ifstream &is) {
  uint8_t firstByte = readByte(is);
  // std::cout << "\n First Byte: " << std::to_string(firstByte) << "\n";

  uint8_t flag = firstByte >> 6;
  uint8_t value = firstByte & 0x3F;
  // std::cout << "\n Flag & Value: " << std::to_string(flag) << " "
  // << std::to_string(value) << "\n";

  if (flag == 0) {
    return value;
  } else if (flag == 1) {
    uint8_t secondByte = readByte(is);
    return value << 8 | secondByte;
  } else if (flag == 2) {
    uint8_t secondByte = readByte(is);
    uint8_t thirdByte = readByte(is);
    uint8_t fourthByte = readByte(is);
    uint8_t fifthByte = readByte(is);
    return fifthByte << 24 | fourthByte << 16 | thirdByte << 8 | secondByte;
  } else if (flag == 3) {
    if (value == 0) {
      uint8_t secondByte = readByte(is);
      return secondByte;
    } else if (value == 1) {
      uint8_t secondByte = readByte(is);
      uint8_t thirdByte = readByte(is);
      return thirdByte << 8 | secondByte;
    } else if (value == 2) {
      uint8_t secondByte = readByte(is);
      uint8_t thirdByte = readByte(is);
      uint8_t fourthByte = readByte(is);
      uint8_t fifthByte = readByte(is);
      return fifthByte << 24 | fourthByte << 16 | thirdByte << 8 | secondByte;
    }
  }
  return -1;
}

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
          // std::cout << "\2nd Argument from client:"
          //           << strcasecmp(message.elements[1].value.c_str(), "get")
          //           << "\n";
          if (message.elements.size() >= 2 &&
              strcasecmp(message.elements[1].value.c_str(), "get") == 0) {
            if (message.elements[2].value == "dir") {
              response = "*2\r\n$3\r\ndir\r\n$" + std::to_string(dir.size()) +
                         "\r\n" + dir + "\r\n";
            } else if (message.elements[2].value == "dbfilename") {
              response = "*2\r\n$10\r\ndbfilename\r\n$" +
                         std::to_string(dbfilename.size()) + "\r\n" +
                         dbfilename + "\r\n";
            }
          }
        } else if (command == "keys") {
          // assume that "*" is passed in.

          // read the file
          std::ifstream is(dir + "/" + dbfilename);

          // identify keys segment

          // skip header section
          char header[9];
          is.read(header, 9);
          std::unordered_map<std::string, std::string> keyValue;

          // process segments
          while (true) {
            uint8_t opcode = readByte(is);
            std::cout << "\nOpcode: " << std::to_string(opcode) << "\n";
            // metadata section
            if (opcode == 0xFA) {
              int length = readLength(is);
              std::cout << "\n Metadata Name Length: " << length << "\n";
              char name[length];
              is.read(name, length);

              length = readLength(is);
              std::cout << "\n Metadata Value Length: " << length << "\n";
              char value[length];
              is.read(value, length);
            } else if (opcode == 0xFE) {
              int length = readLength(is);
              char index[length];
              is.read(index, length);
            } else if (opcode == 0xFB) {
              int length = readLength(is);
              char keyValueHashSize[length];
              is.read(keyValueHashSize, length);

              length = readLength(is);
              char expiryHash[length];
              is.read(expiryHash, length);
            } else if (opcode == 0x00) {
              std::cout << "\nThis ran!\n";
              int length = readLength(is);
              std::string key(length, '\0');
              is.read(&key[0], length);

              length = readLength(is);
              std::string val(length, '\0');
              is.read(&val[0], length);

              keyValue[key] = val;
            } else if (opcode == 0xFC) {
              unsigned long time = 0;
              for (int i = 0; i < 8; i++) {
                uint8_t byte = readByte(is);
                time <<= 8;
                time |= byte;
              }
            } else if (opcode == 0xFD) {
              unsigned int time = 0;
              for (int i = 0; i < 4; i++) {
                uint8_t byte = readByte(is);
                time <<= 8;
                time |= byte;
              }
            } else if (opcode == 0xFF) {
              char checksum[8];
              readBytes(is, checksum, 8);
              break;
            }
          }

          // pull that out
          response = "*" + std::to_string(keyValue.size()) + "\r\n";

          for (auto elem : keyValue) {
            std::string key = elem.first;
            response +=
                "$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
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
