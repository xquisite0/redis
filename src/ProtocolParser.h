#include <iostream>
#include <vector>

enum MessageType { SIMPLE_STRING, SIMPLE_ERROR, INTEGER, BULK_STRING, ARRAY };

struct RedisMessage {
  MessageType type;
  std::string value;
  std::vector<RedisMessage> elements;

  RedisMessage(MessageType t, const std::string &v = "",
               const std::vector<RedisMessage> &e = std::vector<RedisMessage>())
      : type(t), value(v), elements(e) {}
}

;

class ProtocolParser {
public:
  int cursor = 0;
  RedisMessage parse(char *message);

private:
  RedisMessage parseMessage(char *message, int &pos);
  RedisMessage parseSimpleString(char *message, int &pos);
  RedisMessage parseSimpleError(char *message, int &pos);
  RedisMessage parseInteger(char *message, int &pos);
  RedisMessage parseBulkString(char *message, int &pos);
  RedisMessage parseArray(char *message, int &pos);
};