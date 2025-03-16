#include "ProtocolParser.h"
#include "util.cpp"
#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

std::string master_replid = "8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb";
int master_repl_offset = 0;
std::vector<int> replicaSockets;

// key should map should some sort of data structure for entries
std::unordered_set<std::string> streamKeys;

// stream_key -> entries
// each entry is a id, and a list of its key-value pairs represented in a vector
std::unordered_map<
    std::string, std::vector<std::pair<std::string, std::vector<std::string>>>>
    streams;
std::unordered_map<std::string, std::string> keyValue;
bool propagated = false;
int master_fd = -1;
int replica_offset = 0;
std::mutex mtx;
int syncedReplicas = 0;
long long maxMillisecondsTime = 0;
int maxSequenceNumber = 1;

std::pair<long long, int>
extractMillisecondsAndSequence(std::string entry_id, std::string stream_key) {
  std::string millisecondsTimeString = "", sequenceNumberString = "";
  bool isMillisecondsPart = true;
  for (char &c : entry_id) {
    if (c == '-') {
      isMillisecondsPart = false;
      continue;
    }
    if (isMillisecondsPart)
      millisecondsTimeString += c;
    else
      sequenceNumberString += c;
  }

  if (millisecondsTimeString == "*") {
    auto now = std::chrono::system_clock::now();

    // Convert to duration since epoch
    millisecondsTimeString =
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           now.time_since_epoch())
                           .count());
  }

  if (sequenceNumberString == "*" || entry_id == "*") {
    int generatedSequenceNumber = 0;
    if (millisecondsTimeString == "0")
      generatedSequenceNumber = 1;

    if (!streams[stream_key].empty()) {
      auto [prevMillisecondsTime, prevSequenceNumber] =
          extractMillisecondsAndSequence(streams[stream_key].back().first,
                                         stream_key);
      if (prevMillisecondsTime == std::stoll(millisecondsTimeString)) {
        generatedSequenceNumber = prevSequenceNumber + 1;
      }
    }

    sequenceNumberString = std::to_string(generatedSequenceNumber);
  }

  long long millisecondsTime = std::stoll(millisecondsTimeString);
  int sequenceNumber = std::stoi(sequenceNumberString);
  return std::make_pair(millisecondsTime, sequenceNumber);
}

static void readBytes(std::ifstream &is, char *buffer, int length) {
  if (!is.read(buffer, length)) {
    throw std::runtime_error("Unexpected EOF when reading RDB file");
  }
}

static uint8_t readByte(std::ifstream &is) {
  char byte;
  if (!is.read(&byte, 1)) {
    throw std::runtime_error("Unexpected EOF when reading RDB file");
  }
  return static_cast<uint8_t>(byte);
}

int readLength(std::ifstream &is, bool &isValue) {
  uint8_t firstByte = readByte(is);

  uint8_t flag = (firstByte & 0xC0) >> 6;
  uint8_t value = firstByte & 0x3F;

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
    isValue = true;
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

void parseRDB(
    std::unordered_map<std::string, std::string> &keyValue,
    std::unordered_map<std::string, unsigned long long> &keyStartExpiry,
    std::string dir, std::string dbfilename) {

  if (dir == "" && dbfilename == "")
    return;
  // read the file
  std::ifstream is(dir + "/" + dbfilename);
  if (!is.is_open()) {
    std::cerr << "Error: File could not be opened!" << std::endl;
    return;
  }

  // identify keys segment

  // skip header section
  char header[9];
  is.read(header, 9);

  bool expirySet = false;
  unsigned long long expiryTimestamp = -1;
  // process segments
  while (true) {
    uint8_t opcode = readByte(is);

    // metadata section
    if (opcode == 0xFA) {
      bool isValue = false;
      int length = readLength(is, isValue);
      char name[length];
      is.read(name, length);

      isValue = false;
      length = readLength(is, isValue);
      std::string value;
      if (isValue) {
        value = length;
      } else {
        is.read(&value[0], length);
      }
    } else if (opcode == 0xFE) {
      bool isValue = false;
      int databaseIndex = readLength(is, isValue);
    } else if (opcode == 0xFB) {
      bool isValue = false;
      int keyValueHashSize = readLength(is, isValue);

      isValue = false;
      int expiryHashSize = readLength(is, isValue);
    } else if (opcode == 0x00) {

      bool isValue = false;
      int length = readLength(is, isValue);
      std::string key(length, '\0');
      is.read(&key[0], length);

      isValue = false;
      length = readLength(is, isValue);
      std::string val(length, '\0');
      is.read(&val[0], length);

      keyValue[key] = val;

      if (expiryTimestamp != -1) {
        keyStartExpiry[key] = expiryTimestamp;
        expirySet = false;
      }
    } else if (opcode == 0xFC) {
      unsigned long long time = 0;
      for (int i = 0; i < 8; i++) {
        unsigned long long byte = readByte(is);
        time |= (byte << (8 * i));
        // time |= byte;
      }
      expiryTimestamp = time;
      expirySet = true;
    } else if (opcode == 0xFD) {
      unsigned long long time = 0;
      for (int i = 0; i < 4; i++) {
        unsigned long long byte = readByte(is);
        // time <<= 8;
        // time |= byte;
        time |= (byte << (8 * i));
      }
      expiryTimestamp = time * 1000;
      expirySet = true;
    } else if (opcode == 0xFF) {
      char checksum[8];
      readBytes(is, checksum, 8);
      break;
    }
  }
}

std::string receiveResponse(int socketFd) {
  std::string response;
  const size_t bufferSize = 1024; // Buffer size for receiving chunks of data
  char buffer[bufferSize];

  ssize_t bytesReceived;
  bytesReceived = recv(socketFd, buffer, sizeof(buffer) - 1, 0);
  response += buffer;

  if (bytesReceived == 0) {
    std::cout << "Server closed the connection.\n";
  } else if (bytesReceived < 0) {
    std::cerr << "Error receiving data.\n";
  }

  return response;
}

void handleClient(int client_fd, const std::string &dir,
                  const std::string &dbfilename, int port,
                  std::string replicaof, bool isPropagation) {
  std::unordered_map<std::string, unsigned long long> keyStartExpiry;

  bool transactionBegun = false;
  bool transactionExecuting = false;
  std::vector<RedisMessage> transactionCommands;
  std::vector<std::string> transactionResponses;

  // restore state of Redis with persistence.
  parseRDB(keyValue, keyStartExpiry, dir, dbfilename);

  auto now = std::chrono::system_clock::now();

  // Convert to milliseconds since the Unix epoch
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());

  // Get the Unix time in milliseconds
  long long unix_time_ms = duration.count();

  while (true) {

    // check whether the connection is closed by peeking at the top of the
    // buffer
    bool sendResponse = true;

    char buffer;
    if (recv(client_fd, &buffer, 1, MSG_PEEK) <= 0) {
      break;
    }
    std::string response;

    ProtocolParser parser(client_fd);
    parser.reset();
    RedisMessage message = parser.parse();

    if (transactionBegun) {
      std::string command = "";
      for (char &c : message.elements[0].value) {
        command += tolower(c);
      }
      if (command == "discard") {
        transactionCommands.clear();
        transactionBegun = false;
        response = "+OK\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        continue;
      } else if (command != "exec") {
        transactionCommands.push_back(message);
        response = "+QUEUED\r\n";

        send(client_fd, response.c_str(), response.size(), 0);
        continue;
      } else {
        // if (!transactionBegun) {
        //   response = "-ERR EXEC without MULTI\r\n";
        //   send(client_fd, response.c_str(), response.size(), 0);
        // } else {
        if (transactionCommands.empty()) {
          response = "*0\r\n";
          transactionBegun = false;
          send(client_fd, response.c_str(), response.size(), 0);
          continue;
        }

        transactionExecuting = true;
      }
    }

    int transactionNumber = 0;

    do {
      response = "";
      if (transactionExecuting) {
        message = transactionCommands[transactionNumber];
        transactionNumber++;
      }
      // Checking for ECHO command
      if (!message.elements.empty()) {
        RedisMessage firstElement = message.elements[0];
        if (firstElement.type == BULK_STRING) {

          std::string command = "";
          for (char c : firstElement.value) {
            command += tolower(c);
          }
          if (command == "echo") {
            response = "+" + message.elements[1].value + "\r\n";

          } else if (command == "ping") {
            response = "+PONG\r\n";

          } else if (command == "set") {

            master_repl_offset += message.rawMessage.size();
            for (int fd : replicaSockets) {
              send(fd, message.rawMessage.c_str(), message.rawMessage.size(),
                   0);
            }
            keyValue[message.elements[1].value] = message.elements[2].value;

            response = "+OK\r\n";

            if (message.elements.size() > 2) {
              if (message.elements[3].value == "px") {
                auto now = std::chrono::system_clock::now();

                // Convert to milliseconds since the Unix epoch
                auto duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch());

                // Get the Unix time in milliseconds
                long long unix_time_ms = duration.count();
                long long expiryTimestamp =
                    unix_time_ms + stoll(message.elements[4].value);

                keyStartExpiry[message.elements[1].value] = expiryTimestamp;
              }
            }

          } else if (command == "get") {
            bool valid = true;

            // key has not been set
            if (keyValue.find(message.elements[1].value) == keyValue.end()) {
              response = "$-1\r\n";
              valid = false;
            }
            // check for expiry
            if (keyStartExpiry.find(message.elements[1].value) !=
                keyStartExpiry.end()) {
              auto now = std::chrono::system_clock::now();

              // Convert to milliseconds since the Unix epoch
              auto duration =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch());

              // Get the Unix time in milliseconds
              unsigned long long get_time = duration.count();
              unsigned long long expiryTimestamp =
                  keyStartExpiry[message.elements[1].value];
              if (get_time > expiryTimestamp) {
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
            if (strcasecmp(message.elements[1].value.c_str(), "*") == 0) {
              // pull that out
              response = "*" + std::to_string(keyValue.size()) + "\r\n";

              for (auto elem : keyValue) {
                std::string key = elem.first;
                response +=
                    "$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
              }
            }
          } else if (command == "info") {
            // assume that the key is replication

            if (replicaof == "") {
              // std::string offsetstd::to_string(master_repl_offset)
              std::string response_string =
                  "role:master\r\nmaster_replid:" + master_replid +
                  "\r\nmaster_repl_offset:" +
                  std::to_string(master_repl_offset);
              response = "$" + std::to_string(response_string.size()) + "\r\n" +
                         response_string + "\r\n";
            } else {
              response = "$10\r\nrole:slave\r\n";
            }
          } else if (command == "replconf") {
            if (replicaof == "") {
              response = "+OK\r\n";
              if (message.elements.size() >= 3 &&
                  message.elements[1].value == "ACK") {
                sendResponse = false;
                int offset = stoi(message.elements[2].value);

                if (offset == master_repl_offset) {
                  std::unique_lock<std::mutex> lock(mtx); // Lock the mutex
                  ++syncedReplicas;
                }
              }

              /*

              include code here NA2. WAIT with multiple commands.

              */
            } else {
              if (message.elements.size() >= 3) {
                if (message.elements[1].value == "GETACK") {
                  if (message.elements[2].value == "*") {
                    response =
                        "*3\r\n$8\r\nREPLCONF\r\n$3\r\nACK\r\n$" +
                        std::to_string(std::to_string(replica_offset).size()) +
                        "\r\n" + std::to_string(replica_offset) + "\r\n";
                  }
                }
              }
              send(client_fd, response.c_str(), response.size(), 0);
            }
          } else if (command == "psync") {
            response = "+FULLRESYNC " + master_replid + " " +
                       std::to_string(master_repl_offset) + "\r\n";

            send(client_fd, response.c_str(), response.size(), 0);

            // send our RDB file for replica to sync to

            // simulate with an empty RDB file
            std::string emptyRDB =
                "524544495330303131fa0972656469732d76657205372e322e30fa0a726564"
                "69"
                "732d62697473c040fa056374696d65c26d08bc65fa08757365642d6d656dc2"
                "b0"
                "c41000fa08616f662d62617365c000fff06e3bfec0ff5aa2";

            std::string bytes = "";
            for (size_t i = 0; i < emptyRDB.length(); i += 2) {
              std::string byteString = emptyRDB.substr(i, 2);
              unsigned char byte = static_cast<unsigned char>(
                  std::stoi(byteString, nullptr, 16));
              bytes.push_back(byte);
            }

            response = "$" + std::to_string(bytes.size()) + "\r\n" + bytes;

            replicaSockets.push_back(client_fd);
          } else if (command == "wait") {
            int numreplicas = stoi(message.elements[1].value);
            int timeout = stoi(message.elements[2].value);

            if (master_repl_offset == 0) {
              response = ":" + std::to_string(replicaSockets.size()) + "\r\n";
            } else {
              std::string offsetRequest =
                  "*3\r\n$8\r\nREPLCONF\r\n$6\r\nGETACK\r\n$1\r\n*\r\n";

              // while (true) {
              // if (true) {
              std::unique_lock<std::mutex> lock(mtx);
              syncedReplicas = 0;
              lock.unlock();
              // }

              for (int fd : replicaSockets) {
                send(fd, offsetRequest.c_str(), offsetRequest.size(), 0);
              }

              auto startTime = std::chrono::steady_clock::now();

              // Define timeout duration (e.g., 5000 milliseconds = 5 seconds)
              std::chrono::milliseconds timeoutDuration(timeout);

              // Calculate the timeout timestamp
              auto timeoutTimestamp = startTime + timeoutDuration;

              // Loop until the current time reaches the timeout
              while (std::chrono::steady_clock::now() < timeoutTimestamp) {
                std::unique_lock<std::mutex> lock(mtx);
                if (syncedReplicas >= numreplicas)
                  break;
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }

              lock.lock();
              response = ":" + std::to_string(syncedReplicas) + "\r\n";
              master_repl_offset += offsetRequest.size();
            }
          } else if (command == "type") {
            std::string key = message.elements[1].value;

            if (streamKeys.find(key) != streamKeys.end()) {
              response = "+stream\r\n";
            } else if (keyValue.find(key) != keyValue.end()) {
              response = "+string\r\n";
            } else {
              response = "+none\r\n";
            }
          } else if (command == "xadd") {
            std::string stream_key = message.elements[1].value;
            if (streamKeys.find(stream_key) == streamKeys.end()) {
              streamKeys.insert(stream_key);
            }
            if (message.elements.size() >= 3) {
              std::string entry_id = message.elements[2].value;

              // std::pair<long long, int> entry_id_separated =
              // extractMillisecondsAndSequence(entry_id); long long
              // millisecondsTime = entry_id_separated.first; int sequenceNumber
              // = entry_id_separated.second;
              auto [millisecondsTime, sequenceNumber] =
                  extractMillisecondsAndSequence(entry_id, stream_key);

              bool validEntry = true;

              if (millisecondsTime <= 0 && sequenceNumber <= 0) {
                response = "-ERR The ID specified in XADD must be greater than "
                           "0-0\r\n";
                validEntry = false;
              } else if (!streams[stream_key].empty()) {
                auto [prevMillisecondsTime, prevSequenceNumber] =
                    extractMillisecondsAndSequence(
                        streams[stream_key].back().first, stream_key);
                if (prevMillisecondsTime > millisecondsTime ||
                    (prevMillisecondsTime == millisecondsTime &&
                     prevSequenceNumber >= sequenceNumber)) {
                  response =
                      "-ERR The ID specified in XADD is equal or smaller "
                      "than the target stream top item\r\n";
                  validEntry = false;
                }
              }

              // if it is valid, we can add the entry to the stream
              if (validEntry) {
                // refresh entry id, in case it is a generated one. e.g. we
                // don't add id "0-*", we add id "0-1" (auto-generated)
                entry_id = std::to_string(millisecondsTime) + "-" +
                           std::to_string(sequenceNumber);

                if (maxMillisecondsTime > millisecondsTime) {
                  maxMillisecondsTime = millisecondsTime;
                  maxSequenceNumber = sequenceNumber;
                } else if (maxMillisecondsTime == millisecondsTime &&
                           sequenceNumber > maxSequenceNumber) {
                  maxSequenceNumber = sequenceNumber;
                }

                std::pair<std::string, std::vector<std::string>> entry;
                entry.first = entry_id;
                for (int i = 3; i < message.elements.size(); i++) {
                  entry.second.push_back(message.elements[i].value);
                }
                streams[stream_key].push_back(entry);

                response = "$" + std::to_string(entry_id.size()) + "\r\n" +
                           entry_id + "\r\n";
              }
            }
          } else if (command == "xrange") {
            std::string stream_key = message.elements[1].value;

            std::string start = message.elements[2].value,
                        end = message.elements[3].value;

            if (start == "-") {
              start = "0-1";
            }
            if (end == "+") {
              end = std::to_string((long long)1e18) + "-" + std::to_string(1e9);
            }
            if (start.find('-') == std::string::npos)
              start += "-0";
            if (end.find('-') == std::string::npos)
              end += "-" + std::to_string(1e9);

            auto [startMillisecondsTime, startSequenceNumber] =
                extractMillisecondsAndSequence(start, stream_key);
            auto [endMillisecondsTime, endSequenceNumber] =
                extractMillisecondsAndSequence(end, stream_key);

            std::vector<std::pair<std::string, std::vector<std::string>>>
                entriesToOutput;
            for (auto &entry : streams[stream_key]) {
              auto [entry_id, keyValuePairs] = entry;
              auto [curMillisecondsTime, curSequenceNumber] =
                  extractMillisecondsAndSequence(entry_id, stream_key);

              bool afterStart = startMillisecondsTime < curMillisecondsTime ||
                                (startMillisecondsTime == curMillisecondsTime &&
                                 startSequenceNumber <= curSequenceNumber);
              bool beforeEnd = curMillisecondsTime < endMillisecondsTime ||
                               (curMillisecondsTime == endMillisecondsTime &&
                                curSequenceNumber <= endSequenceNumber);

              if (afterStart && beforeEnd) {
                entriesToOutput.push_back(entry);
              }
            }

            // we now have to format the entriesToOutput into RESP format

            response = "*" + std::to_string(entriesToOutput.size()) + "\r\n";
            for (auto &entry : entriesToOutput) {
              response += "*2\r\n";

              auto [entry_id, keyValuePairs] = entry;

              response += "$" + std::to_string(entry_id.size()) + "\r\n" +
                          entry_id + "\r\n";
              response += "*" + std::to_string(keyValuePairs.size()) + "\r\n";

              for (std::string &elem : keyValuePairs) {
                response +=
                    "$" + std::to_string(elem.size()) + "\r\n" + elem + "\r\n";
              }
            }
          } else if (command == "xread") {
            std::vector<std::pair<std::string, std::string>> stream_keys_start;

            int streamsIndexStart = -1;
            long long blockMilliseconds = -1;
            bool entriesPresent = false;
            for (int i = 1; i < message.elements.size(); i++) {
              if (message.elements[i].value == "streams") {
                streamsIndexStart = i + 1;
              }
              if (message.elements[i].value == "block") {
                blockMilliseconds = std::stoll(message.elements[i + 1].value);
              }
            }

            if (blockMilliseconds != -1) {
              std::this_thread::sleep_for(
                  std::chrono::milliseconds(blockMilliseconds));
            }

            int stream_count =
                ((int)message.elements.size() - streamsIndexStart) / 2;
            for (int i = streamsIndexStart;
                 i < streamsIndexStart + stream_count; i++) {
              stream_keys_start.push_back(
                  std::make_pair(message.elements[i].value,
                                 message.elements[stream_count + i].value));
            }
            std::vector<std::pair<
                std::string,
                std::vector<std::pair<std::string, std::vector<std::string>>>>>
                streamsToOutput;
            do {
              streamsToOutput.clear();
              for (auto &[stream_key, start] : stream_keys_start) {

                if (start == "$") {
                  start = std::to_string(maxMillisecondsTime) + "-" +
                          std::to_string(maxSequenceNumber);
                }

                auto [startMillisecondsTime, startSequenceNumber] =
                    extractMillisecondsAndSequence(start, stream_key);
                std::pair<std::string,
                          std::vector<
                              std::pair<std::string, std::vector<std::string>>>>
                    curStream;
                curStream.first = stream_key;

                for (auto &entry : streams[stream_key]) {
                  auto [entry_id, keyValuePairs] = entry;
                  auto [curMillisecondsTime, curSequenceNumber] =
                      extractMillisecondsAndSequence(entry_id, stream_key);

                  bool afterStart =
                      startMillisecondsTime < curMillisecondsTime ||
                      (startMillisecondsTime == curMillisecondsTime &&
                       startSequenceNumber < curSequenceNumber);

                  if (afterStart) {
                    curStream.second.push_back(entry);
                    entriesPresent = true;
                  }
                }
                streamsToOutput.push_back(curStream);
              }
              if (blockMilliseconds != 0 || entriesPresent)
                break;
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } while (blockMilliseconds == 0 && !entriesPresent);

            if (blockMilliseconds != -1 && !entriesPresent) {
              response = "$-1\r\n";
            } else {
              response = "*" + std::to_string(streamsToOutput.size()) + "\r\n";
              for (auto &[stream_key, entries] : streamsToOutput) {
                response += "*2\r\n";
                response += "$" + std::to_string(stream_key.size()) + "\r\n" +
                            stream_key + "\r\n";

                response += "*" + std::to_string(entries.size()) + "\r\n";
                for (auto &[entry_id, keyValuePairs] : entries) {
                  response += "*2\r\n";
                  response += "$" + std::to_string(entry_id.size()) + "\r\n" +
                              entry_id + "\r\n";
                  response +=
                      "*" + std::to_string(keyValuePairs.size()) + "\r\n";

                  for (auto &elem : keyValuePairs) {
                    response += "$" + std::to_string(elem.size()) + "\r\n" +
                                elem + "\r\n";
                  }
                }
              }
            }
          } else if (command == "incr") {
            std::string key = message.elements[1].value;

            // initialise key if it does not exist
            if (keyValue.find(key) == keyValue.end()) {
              keyValue[key] = "0";
            }

            // extract value
            std::string curValueString = keyValue[key];
            int curValue = -1;
            bool validValue = true;

            try {
              curValue = std::stoi(curValueString);
            } catch (...) {
              validValue = false;
            }
            if (!validValue) {
              response = "-ERR value is not an integer or out of range\r\n";
            } else {
              // increment value
              curValue++;

              // replace value
              curValueString = std::to_string(curValue);
              keyValue[key] = curValueString;

              response = ":" + curValueString + "\r\n";
            }
          } else if (command == "multi") {
            transactionBegun = true;
            transactionCommands.clear();
            transactionResponses.clear();
            response = "+OK\r\n";
          } else if (command == "exec") {
            if (!transactionBegun) {
              response = "-ERR EXEC without MULTI\r\n";
            }
          } else if (command == "discard") {
            response = "-ERR DISCARD without MULTI\r\n";
          }
        }
      }

      if (client_fd == master_fd) {
        replica_offset += message.rawMessage.size();
      }
      if (transactionExecuting) {
        transactionResponses.push_back(response);
      }

    } while (transactionExecuting &&
             transactionNumber < transactionCommands.size());

    if (transactionExecuting) {
      response = "*" + std::to_string(transactionResponses.size()) + "\r\n";
      for (std::string &transactionResponse : transactionResponses) {
        response += transactionResponse;
      }
      transactionExecuting = false;
      transactionBegun = false;
    }

    if (client_fd != master_fd && sendResponse) {
      send(client_fd, response.c_str(), response.size(), 0);
    }

    // }

    // if (isPropagation && replicaof != "") {
    //   propagated = true;
    // }
  }
  close(client_fd);
}

void executeHandshake(const std::string &dir, const std::string &dbfilename,
                      int port, std::string replicaof) {
  std::string master_host_string = replicaof.substr(0, replicaof.find(' '));
  if (master_host_string == "localhost") {
    master_host_string = "127.0.0.1";
  }
  in_addr_t MASTER_HOST = inet_addr(master_host_string.c_str());
  std::string master_port_string = replicaof.substr(replicaof.find(' ') + 1);
  int MASTER_PORT = stoi(master_port_string);

  int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
  master_fd = clientSocket;

  struct sockaddr_in master_addr;
  master_addr.sin_family = AF_INET;
  master_addr.sin_addr.s_addr = MASTER_HOST;
  master_addr.sin_port = htons(MASTER_PORT);

  if (connect(clientSocket, (struct sockaddr *)&master_addr,
              sizeof(master_addr)) < 0) {
    perror("Connection failed");
    close(clientSocket);
    return;
  }

  std::string message = "*1\r\n$4\r\nPING\r\n";

  // Send PING message
  send(clientSocket, message.c_str(), message.size(), 0);

  std::string response = receiveResponse(clientSocket);

  message = "*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$" +
            std::to_string(std::to_string(port).size()) + "\r\n" +
            std::to_string(port) + "\r\n";

  send(clientSocket, message.c_str(), message.size(), 0);
  response = receiveResponse(clientSocket);

  message = "*3\r\n$8\r\nREPLCONF\r\n$4\r\ncapa\r\n$6\r\npsync2\r\n";

  send(clientSocket, message.c_str(), message.size(), 0);

  response = receiveResponse(clientSocket);

  message = "*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n";

  send(clientSocket, message.c_str(), message.size(), 0);

  // extract RDB File
  ProtocolParser parser(clientSocket);

  // should be the fullresync message
  RedisMessage parsedResponse = parser.parse();

  // should be the RDB file
  parser.reset();
  parser.isRDB = true;
  parsedResponse = parser.parse();
  parser.isRDB = false;

  std::thread(handleClient, clientSocket, dir, dbfilename, port, replicaof,
              true)
      .detach();
  // handleClient(clientSocket, dir, dbfilename, port, replicaof);
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  std::string dir = "";
  std::string dbfilename = "";
  int port = 6379;
  std::string replicaof = "";

  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--dir") {
      dir = argv[i + 1];
    }
    if (std::string(argv[i]) == "--dbfilename") {
      dbfilename = argv[i + 1];
    }
    if (std::string(argv[i]) == "--port") {
      port = stoi(std::string(argv[i + 1]));
    }
    if (std::string(argv[i]) == "--replicaof") {
      replicaof = std::string(argv[i + 1]);
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
  server_addr.sin_port = htons(port);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) !=
      0) {
    std::cerr << "Failed to bind to port " << port << "\n";
    return 1;
  }

  // bind to master
  int clientSocket = -1;
  if (replicaof != "") {
    std::thread(executeHandshake, dir, dbfilename, port, replicaof).detach();
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
      std::thread(handleClient, client_fd, dir, dbfilename, port, replicaof,
                  false)
          .detach();
    }
  }

  if (clientSocket >= 0)
    close(clientSocket);
  close(server_fd);

  return 0;
}
