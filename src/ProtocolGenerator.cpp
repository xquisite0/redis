#include "../include/ProtocolGenerator.h"

namespace ProtocolGenerator {
std::string createSimpleString(std::string str) { return "+" + str + "\r\n"; }

std::string createNullBulkString() { return "$-1\r\n"; }

std::string createBulkString(std::string str) {
  return "$" + std::to_string(str.size()) + "\r\n" + str + "\r\n";
}

std::string createArray(std::vector<std::string> strs, bool isPureStrings = 1) {
  std::string response = "*" + std::to_string(strs.size()) + "\r\n";

  for (std::string &str : strs) {
    if (!isPureStrings && str[0] == '*') {
      response += str;
    }
    response += createBulkString(str);
  }
  return response;
}

std::string createInteger(std::string integer) {
  return ":" + integer + "\r\n";
}

std::string createErrorMessage(std::string str) {
  return "-ERR " + str + "\r\n";
}
} // namespace ProtocolGenerator