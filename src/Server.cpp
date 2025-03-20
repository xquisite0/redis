#include "../include/ProtocolGenerator.h"
#include "../include/util.h"
#include "ProtocolParser.h"
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

// replication ID of the master. hard coded for test purposes.
std::string master_replid = "8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb";
// keeps track of the number of bytes of propagated commands
int master_repl_offset = 0;
// a list of all sockets that refer to the current master server
std::vector<int> replicaSockets;

std::unordered_set<std::string> streamKeys;

// stream_key -> entries -> entry (id, key-value pairs)
// each entry is a id, and a list of its key-value pairs represented in a vector
std::unordered_map<
    std::string, std::vector<std::pair<std::string, std::vector<std::string>>>>
    streams;

// our main data structure for regular SET & state changing commands
std::unordered_map<std::string, std::string> keyValue;
int master_fd = -1;
int replica_offset = 0;

// mtx governs the read & writing of syncedReplicas
std::mutex mtx;
int syncedReplicas = 0;

// for streams processing with XREAD's $ argument.
// Allows us to check for any new stream entries after the XREAD command.
long long maxMillisecondsTime = 0;
int maxSequenceNumber = 1;

// our main function: Helps to keep handle all Command Processing
void handleClient(int client_fd, const std::string &dir,
                  const std::string &dbfilename, int port,
                  std::string replicaof, bool isPropagation) {

  std::unordered_map<std::string, unsigned long long> keyStartExpiry;

  // following variables help with transaction processing.
  bool transactionBegun = false;
  bool transactionExecuting = false;
  std::vector<RedisMessage> transactionCommands;
  std::vector<std::string> transactionResponses;

  // restore state of Redis with persistence.
  parseRDB(keyValue, keyStartExpiry, dir, dbfilename);

  // Get the Unix time in milliseconds
  long long unix_time_ms = getCurrentUnixTimestamp();

  // Loop allows us to handle commands in streams.
  while (true) {

    bool sendResponse = true;

    // check whether the connection is closed by peeking at the top of the
    // buffer.
    char buffer;
    if (recv(client_fd, &buffer, 1, MSG_PEEK) <= 0) {
      break;
    }

    // variable to store our response.
    std::string response;

    // Parses the client's message in Redis Serialisation Protocol (RESP) format
    // to plain text.
    ProtocolParser parser(client_fd);
    parser.reset();
    RedisMessage message = parser.parse();

    // if transaction has begun, i.e. MULTI has been called, but EXEC hasn't. we
    // are gathering the commands to be batch executed in the future when EXEC
    // is called.
    if (transactionBegun) {

      // first extract the command
      std::string command = "";
      for (char &c : message.elements[0].value) {
        command += tolower(c);
      }

      // DISCARD means that we clear our commands and end the transaction
      // immediately.
      if (command == "discard") {
        transactionCommands.clear();
        transactionBegun = false;
        response = "+OK\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        continue;

        // A regular command will be pushed into our transactionCommands queue
        // to be batch processed in the future.
      } else if (command != "exec") {
        transactionCommands.push_back(message);
        response = "+QUEUED\r\n";

        send(client_fd, response.c_str(), response.size(), 0);
        continue;

        // we have an EXEC command -> batch execute all commands in
        // transactionCommands.
      } else {

        // ensures that we do have commands to process in the first place
        if (transactionCommands.empty()) {
          response = "*0\r\n";
          transactionBegun = false;
          send(client_fd, response.c_str(), response.size(), 0);
          continue;
        }

        // toggles transaction executing mode.
        transactionExecuting = true;
      }
    }

    // keeps track of which command in the transaction we are processing.
    int transactionNumber = 0;

    // do while loop ensures that if no transaction is happening, the current
    // client command will still be processed once and if a transaction is
    // happening, the while condition kicks in.
    do {
      response = "";

      // grabs the next transaction command, if we are in transaction executing
      // mode.
      if (transactionExecuting) {
        message = transactionCommands[transactionNumber];
        transactionNumber++;
      }

      if (!message.elements.empty()) {
        RedisMessage firstElement = message.elements[0];
        if (firstElement.type == BULK_STRING) {

          std::string command = "";
          for (char c : firstElement.value) {
            command += tolower(c);
          }

          // ECHO just needs us to return the 1st argument
          if (command == "echo") {
            response = ProtocolGenerator::createSimpleString(
                message.elements[1].value);

            // response for PING is PONG
          } else if (command == "ping") {
            response = ProtocolGenerator::createSimpleString("PONG");

          } else if (command == "set") {

            // ensure that we propagate this command to all of our replicas to
            // keep state synchronised.
            master_repl_offset += message.rawMessage.size();
            for (int fd : replicaSockets) {
              send(fd, message.rawMessage.c_str(), message.rawMessage.size(),
                   0);
            }

            std::string key = message.elements[1].value,
                        value = message.elements[2].value;
            keyValue[key] = value;

            response = ProtocolGenerator::createSimpleString("OK");

            // handles the case where an expiry timeout is given for this SET
            // key-value pair.
            if (message.elements.size() > 2) {
              if (message.elements[3].value == "px") {
                // Get the Unix time in milliseconds & calculate the expiry
                // timestamp
                long long unix_time_ms = getCurrentUnixTimestamp();
                long long expiryTimestamp =
                    unix_time_ms + stoll(message.elements[4].value);

                // all get commands for this key-value pair, past this expiry
                // timestamp will return NULL.
                keyStartExpiry[message.elements[1].value] = expiryTimestamp;
              }
            }

          } else if (command == "get") {
            bool valid = true;

            // key has not been set
            if (keyValue.find(message.elements[1].value) == keyValue.end()) {
              response = ProtocolGenerator::createNullBulkString();
              valid = false;
            }

            // check for expiry, if expired, the key no longer exists and we
            // return null.
            if (keyStartExpiry.find(message.elements[1].value) !=
                keyStartExpiry.end()) {

              // Get the Unix time in milliseconds
              unsigned long long get_time = getCurrentUnixTimestamp();
              unsigned long long expiryTimestamp =
                  keyStartExpiry[message.elements[1].value];
              if (get_time > expiryTimestamp) {
                response = ProtocolGenerator::createNullBulkString();
                valid = false;
              }
            }

            // if key exists + has not timed out we can return the value.
            if (valid) {
              std::string value = keyValue[message.elements[1].value];
              response = ProtocolGenerator::createBulkString(value);
            }
          } else if (command == "config") {
            // CONFIG GET -> requires us to output the directory & dbfilename of
            // RDB files that we can reference to synchronise our state with
            // what it was previously
            if (message.elements.size() >= 2 &&
                strcasecmp(message.elements[1].value.c_str(), "get") == 0) {
              if (message.elements[2].value == "dir") {
                response = ProtocolGenerator::createArray({"dir", dir});
              } else if (message.elements[2].value == "dbfilename") {
                response =
                    ProtocolGenerator::createArray({"dbfilename", dbfilename});
              }
            }
            // KEYS -> return all the keys in storage
          } else if (command == "keys") {
            // assume that "*" is passed in.
            if (strcasecmp(message.elements[1].value.c_str(), "*") == 0) {
              // pull that out
              std::vector<std::string> keys;

              for (auto elem : keyValue) {
                std::string key = elem.first;
                keys.emplace_back(key);
              }
              response = ProtocolGenerator::createArray(keys);
            }

            // INFO replication -> returns information about the server
          } else if (command == "info") {

            // for master servers, we return the role, replication ID &
            // replication offset.
            if (replicaof == "") {
              response = ProtocolGenerator::createBulkString(
                  "role:master\r\nmaster_replid:" + master_replid +
                  "\r\nmaster_repl_offset:" +
                  std::to_string(master_repl_offset));

              // for replica servers, we just return the role
            } else {
              response = ProtocolGenerator::createBulkString("role:slave");
            }

          } else if (command == "replconf") {

            // for master servers
            if (replicaof == "") {

              // in general, replconf is sent as part of the replica-master
              // handshake. and the default response is OK.
              response = ProtocolGenerator::createSimpleString("OK");

              // however, other times, we could be sending a REPLCONF GETACK to
              // our replicas replicas will then return a REPLCONF ACK
              // replica_offset to us. we can use this replica_offset to track
              // the total number of replicas that are fully synced with our
              // master server
              if (message.elements.size() >= 3 &&
                  message.elements[1].value == "ACK") {

                // we do not need to reply to replica's response
                sendResponse = false;
                int offset = stoi(message.elements[2].value);

                // we check whether offset (number of bytes of propagated
                // commands PROCESSED & HANDLED by the replica)... is equal to
                // master_repl_offset (number of bytes of commands PROPAGATED by
                // the master to the replicas)
                if (offset == master_repl_offset) {
                  // we lock the mutex that is controlling the syncedReplicas
                  // variable as there are multiple replica-master connections
                  // happening in parallel. this prevents any race conditions.
                  std::unique_lock<std::mutex> lock(mtx);
                  ++syncedReplicas;
                }
              }

              // in this case, we are a replica handling the REPLCONF GETACK *
              // command, which.. needs us to respond with our replica_offset,
              // i.e. number of bytes of propagated commands from the master
              // PROCESSED & HANDLED by us.
            } else {
              if (message.elements.size() >= 3) {
                if (message.elements[1].value == "GETACK") {
                  if (message.elements[2].value == "*") {

                    response = ProtocolGenerator::createArray(
                        {"REPLCONF", "ACK", std::to_string(replica_offset)});
                  }
                }
              }
              // by default, replicas usually don't send responses when
              // processing propagated commands from the master. however, this
              // is an exception, when the master deliberately wants a response
              // from us, hence the explicit socket send().
              send(client_fd, response.c_str(), response.size(), 0);
            }

            // PSYNC -> part of the master-replica handshake process.
            // PSYNC is sent by the replica as a request to synchronise states
            // with the master the master (us now) will respond with FULLRESYNC
            // & a RDB file (empty for test purposes) that the newly created
            // replica can use to synchronise its state with the master
          } else if (command == "psync") {
            response = ProtocolGenerator::createSimpleString(
                "FULLRESYNC " + master_replid + " " +
                std::to_string(master_repl_offset));

            // explicit send, as we have to send the RDB file as well in this
            // same command.
            send(client_fd, response.c_str(), response.size(), 0);

            // send our RDB file for replica to sync to

            // simulate with an empty RDB file
            std::string emptyRDB =
                "524544495330303131fa0972656469732d76657205372e322e30fa0a726564"
                "69"
                "732d62697473c040fa056374696d65c26d08bc65fa08757365642d6d656dc2"
                "b0"
                "c41000fa08616f662d62617365c000fff06e3bfec0ff5aa2";

            // converts hex RDB file to binary
            std::string bytes = "";
            for (size_t i = 0; i < emptyRDB.length(); i += 2) {
              std::string byteString = emptyRDB.substr(i, 2);
              unsigned char byte = static_cast<unsigned char>(
                  std::stoi(byteString, nullptr, 16));
              bytes.push_back(byte);
            }

            // NOT a RESP bulk string (see the ommission of \r\n at the end). we
            // can't use our ProtocolGenerator here.
            response = "$" + std::to_string(bytes.size()) + "\r\n" + bytes;

            // handshake process is completed at this step! we can add this
            // replica to our list of replicaSocket :)
            replicaSockets.push_back(client_fd);

            // WAIT -> command used by clients on the master
            // WAIT will wait for a minimum number of replicas (numreplicas) to
            // be fully synchronised to our master before returning and
            // continuing command execution OR it timeouts. WAIT returns the
            // number of replicas synced, be it above numreplicas or under. WAIT
            // creates a safety net by ensuring a certain number of replicas are
            // in sync.

            /*
            Key details from the Redis Docs on WAIT:

            Note that WAIT does not make Redis a strongly consistent store:
            while synchronous replication is part of a replicated state machine,
            it is not the only thing needed. However in the context of Sentinel
            or Redis Cluster failover, WAIT improves the real world data safety.

            Specifically if a given write is transferred to one or more
            replicas, it is more likely (but not guaranteed) that if the master
            fails, we'll be able to promote, during a failover, a replica that
            received the write: both Sentinel and Redis Cluster will do a
            best-effort attempt to promote the best replica among the set of
            available replicas.

            However this is just a best-effort attempt so it is possible to
            still lose a write synchronously replicated to multiple replicas.

            */
          } else if (command == "wait") {

            // extract arguments
            int numreplicas = stoi(message.elements[1].value);
            int timeout = stoi(message.elements[2].value);

            // if the master has not propagated any commands, all replicas by
            // default are in sync!
            if (master_repl_offset == 0) {
              response = ProtocolGenerator::createInteger(
                  std::to_string(replicaSockets.size()));
            } else {
              std::string offsetRequest =
                  ProtocolGenerator::createArray({"REPLCONF", "GETACK", "*"});

              // reset the syncedReplicas count, as new commands may have been
              // propagated between different instances of WAIT.
              std::unique_lock<std::mutex> lock(mtx);
              syncedReplicas = 0;
              lock.unlock();

              // request the replica_offset from all replicas.
              // syncedReplicas will be updated on each particular
              // Master-Replica connection when the replica returns with
              // REPLCONF ACK replica_offset to the Master.
              // Take note that currently, this is running on the Master-Client
              // connection... The Master-Replica connections are running
              // concurrently to this connection!
              for (int fd : replicaSockets) {
                send(fd, offsetRequest.c_str(), offsetRequest.size(), 0);
              }

              // caculates the timeout timestamp
              auto startTime = std::chrono::steady_clock::now();
              std::chrono::milliseconds timeoutDuration(timeout);
              auto timeoutTimestamp = startTime + timeoutDuration;

              // Recheck the syncedReplicas count every 0.1s to check if it is
              // sufficient. If so, we can return the syncedReplicas count
              // immediately Checks until timeout.
              while (std::chrono::steady_clock::now() < timeoutTimestamp) {
                std::unique_lock<std::mutex> lock(mtx);
                if (syncedReplicas >= numreplicas)
                  break;
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }

              // Read guard.
              lock.lock();
              response = ProtocolGenerator::createInteger(
                  std::to_string(syncedReplicas));

              // requesting the replica_offset from each replica is itself a
              // propagated command too.
              master_repl_offset += offsetRequest.size();
            }

            // TYPE -> return the type of value a key is.
          } else if (command == "type") {
            std::string key = message.elements[1].value;

            if (streamKeys.find(key) != streamKeys.end()) {
              response = ProtocolGenerator::createSimpleString("stream");
            } else if (keyValue.find(key) != keyValue.end()) {
              response = ProtocolGenerator::createSimpleString("string");
            } else {
              response = ProtocolGenerator::createSimpleString("none");
            }

            // XADD -> appends a specified stream entry to a stream
          } else if (command == "xadd") {
            std::string stream_key = message.elements[1].value;

            // we create the specified stream, if it has not yet been created.
            if (streamKeys.find(stream_key) == streamKeys.end()) {
              streamKeys.insert(stream_key);
            }

            if (message.elements.size() >= 3) {

              // extract the millisecondsTime & sequenceNumber of the current
              // stream entry ID.
              std::string entry_id = message.elements[2].value;
              auto [millisecondsTime, sequenceNumber] =
                  extractMillisecondsAndSequence(entry_id, stream_key, streams);

              bool validEntry = true;

              // verifies the validity of the entry ID of the current stream
              // entry
              if (millisecondsTime <= 0 && sequenceNumber <= 0) {
                response = ProtocolGenerator::createErrorMessage(
                    "The ID specified in XADD must be greater than 0-0");
                validEntry = false;
              } else if (!streams[stream_key].empty()) {

                // checks that the current stream entry is chronologically after
                // the previous stream entry.
                auto [prevMillisecondsTime, prevSequenceNumber] =
                    extractMillisecondsAndSequence(
                        streams[stream_key].back().first, stream_key, streams);
                if (prevMillisecondsTime > millisecondsTime ||
                    (prevMillisecondsTime == millisecondsTime &&
                     prevSequenceNumber >= sequenceNumber)) {
                  response = ProtocolGenerator::createErrorMessage(
                      "The ID specified in XADD is equal or smaller than the "
                      "target stream top item");
                  validEntry = false;
                }
              }

              // if it is valid, we can add the entry to the stream
              if (validEntry) {
                // there are cases where we are passed entry IDs like "*-*" into
                // XADD. in these cases, extractMillisecondsAndSequence helps to
                // generate suitable millisecondsTime & sequenceNumber values.
                // hence, the need to "refresh" the entry_id value to account
                // for these auto-generated cases.
                entry_id = std::to_string(millisecondsTime) + "-" +
                           std::to_string(sequenceNumber);

                // keep track of the maxMillisecondsTime & maxSequenceNumber for
                // XREAD's $ argument.
                if (maxMillisecondsTime > millisecondsTime) {
                  maxMillisecondsTime = millisecondsTime;
                  maxSequenceNumber = sequenceNumber;
                } else if (maxMillisecondsTime == millisecondsTime &&
                           sequenceNumber > maxSequenceNumber) {
                  maxSequenceNumber = sequenceNumber;
                }

                // entry format -> {entry_id, key value pairs in the entry}
                std::pair<std::string, std::vector<std::string>> entry;
                entry.first = entry_id;
                for (int i = 3; i < message.elements.size(); i++) {
                  entry.second.push_back(message.elements[i].value);
                }
                streams[stream_key].push_back(entry);

                response = ProtocolGenerator::createBulkString(entry_id);
              }
            }
            // XRANGE -> outputs the stream entries from a particular stream in
            // a whose entry IDs range between start & end
          } else if (command == "xrange") {
            std::string stream_key = message.elements[1].value;

            std::string start = message.elements[2].value,
                        end = message.elements[3].value;

            // we start from the minimum entry ID.
            if (start == "-") {
              start = "0-1";
            }

            // we end at the maximum entry ID.
            if (end == "+") {
              end = std::to_string((long long)1e18) + "-" + std::to_string(1e9);
            }

            // we auto generate sequence number values, should they be omitted.
            if (start.find('-') == std::string::npos)
              start += "-0";
            if (end.find('-') == std::string::npos)
              end += "-" + std::to_string(1e9);

            auto [startMillisecondsTime, startSequenceNumber] =
                extractMillisecondsAndSequence(start, stream_key, streams);
            auto [endMillisecondsTime, endSequenceNumber] =
                extractMillisecondsAndSequence(end, stream_key, streams);

            std::vector<std::pair<std::string, std::vector<std::string>>>
                entriesToOutput;

            // iterate over every entry, and check whether their IDs fall into
            // the range between start and end.
            for (auto &entry : streams[stream_key]) {
              auto [entry_id, keyValuePairs] = entry;
              auto [curMillisecondsTime, curSequenceNumber] =
                  extractMillisecondsAndSequence(entry_id, stream_key, streams);

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
            std::vector<std::string> entriesString;
            for (auto &entry : entriesToOutput) {

              auto [entry_id, keyValuePairs] = entry;

              std::string keyValuePairsString =
                  ProtocolGenerator::createArray(keyValuePairs);

              std::string entryString = ProtocolGenerator::createArray(
                  {entry_id, keyValuePairsString}, 0);
              entriesString.emplace_back(entryString);
            }
            response = ProtocolGenerator::createArray(entriesString, 0);

            // XREAD -> reads entries from 1 or more streams
            // BLOCK argument -> command will block for a specified amount of
            // time blockMilliseconds before retrieving entries $ argument ->
            // command will only return new stream entries past the
            // maxMilliseconds & maxSequenceNumber recorded in the latest entry
            // from XADD based on entry ID.
          } else if (command == "xread") {

            // a list of {stream_key, start entry ID} -> for each list element
            // here, return all entries from stream_key, that is after start
            // entry ID.
            std::vector<std::pair<std::string, std::string>> stream_keys_start;

            // process the command line arguments to get the following
            // 1) a list of stream keys and their corresponding starting entry
            // IDs to return from 2) blockMilliseconds - time to block execution
            // for.
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

            // sleep for blockMilliseconds
            if (blockMilliseconds != -1) {
              std::this_thread::sleep_for(
                  std::chrono::milliseconds(blockMilliseconds));
            }

            // extract stream keys & their corresponding start entry IDs, that
            // we should start looking from in our XREAD, not necessarily their
            // first entries in the stream,
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

            // do while loop to look for any entries every 0.1s.
            // this code block repeats when the block timoeut is 0
            // to make sure that block until at least 1 entry can be returned as
            // per the documentation.
            do {
              streamsToOutput.clear();
              for (auto &[stream_key, start] : stream_keys_start) {

                // $ represents that we will only look for new entries after the
                // XREAD command was given.
                if (start == "$") {
                  start = std::to_string(maxMillisecondsTime) + "-" +
                          std::to_string(maxSequenceNumber);
                }

                auto [startMillisecondsTime, startSequenceNumber] =
                    extractMillisecondsAndSequence(start, stream_key, streams);

                // {stream_key, list of {entry_id, entry key-value pairs}}
                std::pair<std::string,
                          std::vector<
                              std::pair<std::string, std::vector<std::string>>>>
                    curStream;
                curStream.first = stream_key;

                // for the current stream, append entries that are after our
                // starting entryID as specified in the start variable in this
                // scope.
                for (auto &entry : streams[stream_key]) {
                  auto [entry_id, keyValuePairs] = entry;
                  auto [curMillisecondsTime, curSequenceNumber] =
                      extractMillisecondsAndSequence(entry_id, stream_key,
                                                     streams);

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
              response = ProtocolGenerator::createNullBulkString();
            } else {

              // format the output in RESP.
              // response = "*" + std::to_string(streamsToOutput.size()) +
              // "\r\n";
              std::vector<std::string> streamsStringList;
              for (auto &[stream_key, entries] : streamsToOutput) {
                std::vector<std::string> entriesStringList;
                for (auto &[entry_id, keyValuePairs] : entries) {

                  std::string keyValuePairsString =
                      ProtocolGenerator::createArray(keyValuePairs);

                  std::string entryString = ProtocolGenerator::createArray(
                      {entry_id, keyValuePairsString}, 0);
                  entriesStringList.emplace_back(entryString);
                }
                std::string entriesString =
                    ProtocolGenerator::createArray(entriesStringList, 0);

                std::string streamString = ProtocolGenerator::createArray(
                    {stream_key, entriesString}, 0);
                streamsStringList.emplace_back(streamString);
              }
              response = ProtocolGenerator::createArray(streamsStringList, 0);
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

      // if the client we are handling in this case is the Master...
      // then we should update our replica offset, since we processed a
      // propagated command.
      if (client_fd == master_fd) {
        replica_offset += message.rawMessage.size();
      }

      // keeps track of the response in each command in a transaction
      if (transactionExecuting) {
        transactionResponses.push_back(response);
      }

    } while (transactionExecuting &&
             transactionNumber < transactionCommands.size());

    // batch send the transaction responses.
    if (transactionExecuting) {
      response = "*" + std::to_string(transactionResponses.size()) + "\r\n";
      for (std::string &transactionResponse : transactionResponses) {
        response += transactionResponse;
      }
      transactionExecuting = false;
      transactionBegun = false;
    }

    // if we are facing a real client's commands, and not just propagated
    // commands, send a response to the client
    if (client_fd != master_fd && sendResponse) {
      send(client_fd, response.c_str(), response.size(), 0);
    }
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

  // establishes a connection with the client socket with the master socket
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

  // Extract Command Line arguments. dir & dbfilename - RDB files that contain
  // the previous state of the database replicaof - port number of the master
  // server of this replica port - which port number we connect to
  std::string dir, dbfilename, replicaof;
  int port = 6379;
  extractCommandLineArguments(dir, dbfilename, port, replicaof, argc, argv);

  // Instantiate a listening socket to listen for any connections
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

  // Have the replica execute a handshake (back-and-forth pinging)
  // asynchronously, that.. 1) ensures that the replica and master can
  // communicate 2) master can send the current state of the database via a
  // Redis Database (RDB) file and the replica can then sync with the current
  // state of the database first by parsing the RDB file
  if (replicaof != "") {
    std::thread(executeHandshake, dir, dbfilename, port, replicaof).detach();
  }

  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }

  // blocks and awaits for any client connections
  // client connections are then processed independently, concurrently with the
  // handleClient function. This allows for greater availability as multiple
  // clients can use the database at the same time.
  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);
  while (true) {
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr,
                           (socklen_t *)&client_addr_len);

    if (client_fd >= 0) {
      std::thread(handleClient, client_fd, dir, dbfilename, port, replicaof,
                  false)
          .detach();
    }
  }
  close(server_fd);

  return 0;
}
