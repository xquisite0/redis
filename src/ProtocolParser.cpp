#include "ProtocolParser.h"
#include <iostream>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

ProtocolParser::ProtocolParser(int socket_fd) : sockfd(socket_fd) {}

std::string ProtocolParser::readUntilCRLF() {
  std::string result;
  char ch;
  while (recv(sockfd, &ch, 1, 0) > 0) {
    result += ch;
    rawMessage += ch;

    if (result.size() >= 2 && result.substr(result.size() - 2) == "\r\n") {
      result.erase(result.size() - 2);
      break;
    }
  }

  return result;
}

void ProtocolParser::reset() { rawMessage = ""; }

RedisMessage ProtocolParser::parse() {
  char prefix;
  if (recv(sockfd, &prefix, 1, 0) <= 0) {
    std::cout << "\nPrefix is " << prefix << "\n";
    throw std::runtime_error("Error reading prefix from socket");
  }
  rawMessage += prefix;

  switch (prefix) {
  case '+':
    return parseSimpleString();
  case '-':
    return parseSimpleError();
  case ':':
    return parseInteger();
  case '$':
    return parseBulkString();
  case '*':
    return parseArray();
  default:
    throw std::runtime_error("Unknown prefix character");
  }
}

RedisMessage ProtocolParser::parseSimpleString() {
  std::string value = readUntilCRLF();
  return RedisMessage(SIMPLE_STRING, value, std::vector<RedisMessage>(),
                      rawMessage);
}

RedisMessage ProtocolParser::parseSimpleError() {
  std::string value = readUntilCRLF();
  return RedisMessage(SIMPLE_ERROR, value, std::vector<RedisMessage>(),
                      rawMessage);
}

RedisMessage ProtocolParser::parseInteger() {
  std::string value = readUntilCRLF();
  return RedisMessage(INTEGER, value, std::vector<RedisMessage>(), rawMessage);
}

RedisMessage ProtocolParser::parseBulkString() {
  std::string lengthStr = readUntilCRLF();
  int length = std::stoi(lengthStr);
  if (length == -1)
    return RedisMessage(BULK_STRING, "", std::vector<RedisMessage>(),
                        rawMessage);

  std::string value(length, '\0');
  recv(sockfd, &value[0], length, 0);
  rawMessage += value;

  if (isRDB)
    return RedisMessage(BULK_STRING, value, std::vector<RedisMessage>(),
                        rawMessage);

  readUntilCRLF(); // Consume trailing CRLF
  return RedisMessage(BULK_STRING, value, std::vector<RedisMessage>(),
                      rawMessage);
}

RedisMessage ProtocolParser::parseArray() {
  std::string lengthStr = readUntilCRLF();
  int length = std::stoi(lengthStr);

  std::vector<RedisMessage> elements;
  for (int i = 0; i < length; i++) {
    elements.push_back(parse());
  }
  return RedisMessage(ARRAY, "", elements, rawMessage);
}

// #include "ProtocolParser.h"
// #include <iostream>
// #include <string.h>
// #include <vector>

// RedisMessage ProtocolParser::parse(char *message) {
//   return parseMessage(message, cursor);
// }

// RedisMessage ProtocolParser::parseMessage(char *message, int &pos) {
//   if (message[pos] == '+') {
//     return parseSimpleString(message, ++pos);
//   } else if (message[pos] == '-') {
//     return parseSimpleError(message, ++pos);
//   } else if (message[pos] == ':') {
//     return parseInteger(message, ++pos);
//   } else if (message[pos] == '$') {
//     return parseBulkString(message, ++pos);
//   } else if (message[pos] == '*') {
//     return parseArray(message, ++pos);
//   }
// }
// RedisMessage ProtocolParser::parseSimpleString(char *message, int &pos) {
//   std::string value = "";
//   while (message[pos] != '\r') {
//     value += message[pos];
//     pos++;
//   }
//   pos += 2;
//   return RedisMessage(SIMPLE_STRING, value);
// }
// RedisMessage ProtocolParser::parseSimpleError(char *message, int &pos) {
//   std::string value = "";
//   while (message[pos] != '\r') {
//     value += message[pos];
//     pos++;
//   }
//   pos += 2;
//   return RedisMessage(SIMPLE_ERROR, value);
// }
// RedisMessage ProtocolParser::parseInteger(char *message, int &pos) {
//   std::string value = "";
//   while (message[pos] != '\r') {
//     value += message[pos];
//     pos++;
//   }
//   pos += 2;
//   return RedisMessage(INTEGER, value);
// }
// RedisMessage ProtocolParser::parseBulkString(char *message, int &pos) {
//   std::string lengthString = "";
//   while (message[pos] != '\r') {
//     lengthString += message[pos];
//     pos++;
//   }
//   int length = stoi(lengthString);
//   if (length == -1)
//     return RedisMessage(BULK_STRING, "");

//   // onto the string
//   std::string value = "";
//   pos += 2;
//   for (int i = 0; i < length; i++) {
//     value += message[pos];
//     pos++;
//   }
//   pos += 2;
//   return RedisMessage(BULK_STRING, value);
// }
// RedisMessage ProtocolParser::parseArray(char *message, int &pos) {
//   std::string lengthString = "";
//   while (message[pos] != '\r') {
//     lengthString += message[pos];
//     pos++;
//   }
//   int length = stoi(lengthString);

//   pos += 2;

//   std::vector<RedisMessage> elements;
//   for (int i = 0; i < length; i++) {
//     elements.push_back(parseMessage(message, pos));
//   }
//   return RedisMessage(ARRAY, "", elements);
// }