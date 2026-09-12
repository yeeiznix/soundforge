// SoundForge G0 — UUID v4 generation and RFC3339 UTC timestamps.
#include "sf_internal.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <random>

namespace sfcore {
namespace {
std::mt19937_64& rng() {
  static std::mt19937_64 instance{std::random_device{}()};
  return instance;
}
}  // namespace

Uuid uuid_generate() {
  static const char* hex = "0123456789abcdef";
  std::uniform_int_distribution<unsigned int> dist(0, 255);
  unsigned char b[16];
  for (auto& byte : b) byte = static_cast<unsigned char>(dist(rng()));
  b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);  // variant 10xx
  char out[37];
  int p = 0;
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[p++] = '-';
    out[p++] = hex[b[i] >> 4];
    out[p++] = hex[b[i] & 0x0F];
  }
  out[p] = '\0';
  return std::string(out);
}

void now_iso8601_raw(char* buf, size_t cap) {
  using namespace std::chrono;
  const auto tp = system_clock::now();
  const std::time_t t = system_clock::to_time_t(tp);
  const auto ms = static_cast<long>(
      duration_cast<milliseconds>(tp.time_since_epoch()).count() % 1000);
  std::tm tmv{};
#ifdef _WIN32
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  std::snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tmv.tm_year + 1900,
                tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
}

IsoTimestamp now_iso8601() {
  char buf[40];
  now_iso8601_raw(buf, sizeof(buf));
  return std::string(buf);
}

}  // namespace sfcore
