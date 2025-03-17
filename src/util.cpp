#include <iostream>
#include <unordered_map>

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