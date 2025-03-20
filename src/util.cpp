#include "../include/util.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/socket.h>
#include <unordered_map>

// Stream Entry IDs are always in the following format.
// milliseconds-sequenceNumber This function helps to split these 2 values
// apart, generate them if needed, and outputs them in a std::pair<long long,
// int>
std::pair<long long, int> extractMillisecondsAndSequence(
    std::string entry_id, std::string stream_key,
    std::unordered_map<
        std::string,
        std::vector<std::pair<std::string, std::vector<std::string>>>>
        &streams) {
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
                                         stream_key, streams);
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

long long getCurrentUnixTimestamp() {
  auto now = std::chrono::system_clock::now();

  // Convert to milliseconds since the Unix epoch
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());

  // Get the Unix time in milliseconds
  return duration.count();
}

void extractCommandLineArguments(std::string &dir, std::string &dbfilename,
                                 int &port, std::string &replicaof, int argc,
                                 char **argv) {
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
}