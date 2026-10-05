#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <unordered_map>
class CustomHash {
public:
    static uint64_t fnv1a(const std::string& input) {
        const uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
        const uint64_t FNV_PRIME = 1099511628211ULL;
        uint64_t hash = FNV_OFFSET_BASIS;
        for (char c : input) {
            hash ^= static_cast<uint8_t>(c);
            hash *= FNV_PRIME;
        }
        return hash;
    }
    static uint64_t djb2(const std::string& input) {
        uint64_t hash = 5381ULL;
        for (char c : input) {
            hash = ((hash << 5) + hash) + static_cast<uint8_t>(c); // hash * 33 + c
        }
        return hash;
    }
    static std::string hash(const std::string& input) {
        uint64_t h1 = fnv1a(input);
        uint64_t h2 = djb2(input);
        std::ostringstream ss;
        ss << std::hex << std::setfill('0')
           << std::setw(16) << h1
           << std::setw(16) << h2;
        return ss.str();
    }
};
