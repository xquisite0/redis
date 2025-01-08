#include "ProtocolParser.h"
#include <iostream>
#include <vector>

enum MessageType { SIMPLE_STRING, SIMPLE_ERROR, INTEGER, BULK_STRING, ARRAY };

struct RedisMessage {
  MessageType type;
  std::string value;
  std::vector<RedisMessage> elements;
};

class ProtocolParser {
public:
  int cursor = 0;

  RedisMessage parse(char *message) {
    int pos = 0;
    parseMessage(message, pos);
  }

private:
  RedisMessage parseMessage(char *message, int &pos) {
    if (message[pos] == '+') {
      return parseSimpleString(message, ++pos);
    } else if (message[pos] == '-') {
      return parseSimpleError(message, ++pos);
    } else if (message[pos] == ':') {
      return parseInteger(message, ++pos);
    } else if (message[pos] == '$') {
      return parseBulkString(message, ++pos);
    } else if (message[pos] == '*') {
      return parseArray(message, ++pos);
    }
  }
  RedisMessage parseSimpleString(char *message, int &pos) {
    std::string value = "";
    while (message[pos] != '\r') {
      value += message[pos];
      pos++;
    }
    pos += 2;
    return {SIMPLE_STRING, value};
  }
  RedisMessage parseSimpleError(char *message, int &pos) {
    std::string value = "";
    while (message[pos] != '\r') {
      value += message[pos];
      pos++;
    }
    pos += 2;
    return {SIMPLE_ERROR, value};
  }
  RedisMessage parseInteger(char *message, int &pos) {
    std::string value = "";
    while (message[pos] != '\r') {
      value += message[pos];
      pos++;
    }
    pos += 2;
    return {INTEGER, value};
  }
  RedisMessage parseBulkString(char *message, int &pos) {
    std::string lengthString = "";
    while (message[pos] != '\r') {
      lengthString += message[pos];
      pos++;
    }
    int length = stoi(lengthString);
    if (length == -1)
      return {BULK_STRING, ""};

    // onto the string
    std::string value = "";
    pos += 2;
    for (int i = 0; i < length; i++) {
      value += message[pos];
      pos++;
    }
    pos += 2;
    return {BULK_STRING, value};
  }
  RedisMessage parseArray(char *message, int &pos) {
    std::string lengthString = "";
    while (message[pos] != '\r') {
      lengthString += message[pos];
      pos++;
    }
    int length = stoi(lengthString);

    pos += 2;

    std::vector<RedisMessage> elements;
    for (int i = 0; i < length; i++) {
      elements.push_back(parseMessage(message, pos));
    }
    return {ARRAY, "", elements};
  }
};