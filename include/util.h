#ifndef UTIL_H
#define UTIL_H

#include <chrono>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// Function to extract milliseconds and sequence number from an entry ID
std::pair<long long, int> extractMillisecondsAndSequence(
    std::string entry_id, std::string stream_key,
    std::unordered_map<
        std::string,
        std::vector<std::pair<std::string, std::vector<std::string>>>>
        &streams);

// Function that reads length bytes from the specified file
static void readBytes(std::ifstream &is, char *buffer, int length);

int readLength(std::ifstream &is, bool &isValue);

void parseRDB(
    std::unordered_map<std::string, std::string> &keyValue,
    std::unordered_map<std::string, unsigned long long> &keyStartExpiry,
    std::string dir, std::string dbfilename);

std::string receiveResponse(int socketFd);

long long getCurrentUnixTimestamp();
void extractCommandLineArguments(std::string &dir, std::string &dbfilename,
                                 int &port, std::string &replicaof, int argc,
                                 char **argv);

#endif // UTIL_H
