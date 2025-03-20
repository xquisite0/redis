#include <iostream>
#include <string>
#include <vector>

namespace ProtocolGenerator {
std::string createSimpleString(std::string str);

std::string createNullBulkString();

std::string createBulkString(std::string str);

std::string createArray(std::vector<std::string> strs);

std::string createInteger(std::string integer);

std::string createErrorMessage(std::string str);
} // namespace ProtocolGenerator