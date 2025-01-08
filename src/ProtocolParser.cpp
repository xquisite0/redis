#include "ProtocolParser.h"
#include <iostream>
#include <vector>

RedisMessage ProtocolParser::parse(char *message) {
  int pos = 0;
  return parseMessage(message, pos);
}

RedisMessage ProtocolParser::parseMessage(char *message, int &pos) {
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
RedisMessage ProtocolParser::parseSimpleString(char *message, int &pos) {
  std::string value = "";
  while (message[pos] != '\r') {
    value += message[pos];
    pos++;
  }
  pos += 2;
  return RedisMessage(SIMPLE_STRING, value);
}
RedisMessage ProtocolParser::parseSimpleError(char *message, int &pos) {
  std::string value = "";
  while (message[pos] != '\r') {
    value += message[pos];
    pos++;
  }
  pos += 2;
  return RedisMessage(SIMPLE_ERROR, value);
}
RedisMessage ProtocolParser::parseInteger(char *message, int &pos) {
  std::string value = "";
  while (message[pos] != '\r') {
    value += message[pos];
    pos++;
  }
  pos += 2;
  return RedisMessage(INTEGER, value);
}
RedisMessage ProtocolParser::parseBulkString(char *message, int &pos) {
  std::string lengthString = "";
  while (message[pos] != '\r') {
    lengthString += message[pos];
    pos++;
  }
  int length = stoi(lengthString);
  if (length == -1)
    return RedisMessage(BULK_STRING, "");

  // onto the string
  std::string value = "";
  pos += 2;
  for (int i = 0; i < length; i++) {
    value += message[pos];
    pos++;
  }
  pos += 2;
  return RedisMessage(BULK_STRING, value);
}
RedisMessage ProtocolParser::parseArray(char *message, int &pos) {
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
  return RedisMessage(ARRAY, "", elements);
}