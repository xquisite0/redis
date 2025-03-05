#ifndef PROTOCOLPARSER_H
#define PROTOCOLPARSER_H

#include <iostream>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

enum MessageType { SIMPLE_STRING, SIMPLE_ERROR, INTEGER, BULK_STRING, ARRAY };

struct RedisMessage {
  MessageType type;
  std::string value;
  std::vector<RedisMessage> elements;
  std::string rawMessage;

  RedisMessage(MessageType t, const std::string &v = "",
               const std::vector<RedisMessage> &e = std::vector<RedisMessage>(),
               std::string rawMessage = "")
      : type(t), value(v), elements(e), rawMessage(rawMessage) {}
}

;

class ProtocolParser {
public:
  explicit ProtocolParser(int socket_fd);
  RedisMessage parse();
  void reset();
  bool isRDB = false;

private:
  int sockfd;
  std::string rawMessage;
  std::string readUntilCRLF();
  RedisMessage parseMessage();
  RedisMessage parseSimpleString();
  RedisMessage parseSimpleError();
  RedisMessage parseInteger();
  RedisMessage parseBulkString();
  RedisMessage parseArray();
};

#endif // PROTOCOLPARSER_H