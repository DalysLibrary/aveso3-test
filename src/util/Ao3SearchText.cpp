#include "Ao3SearchText.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace ao3text {

size_t formatLine(char* dst, size_t cap, uint32_t hash, const char* path, const char* text) {
  if (!dst || cap < 12) return 0;
  const char* p = path ? path : "";
  const char* t = text ? text : "";
  const size_t need = 8 + 1 + strlen(p) + 1 + strlen(t) + 1;
  if (need + 1 > cap) return 0;

  static const char kHex[] = "0123456789abcdef";
  size_t n = 0;
  for (int shift = 28; shift >= 0; shift -= 4) dst[n++] = kHex[(hash >> shift) & 0xF];
  dst[n++] = '\t';
  const auto copyClean = [&](const char* s) {
    for (; *s; s++) dst[n++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
  };
  copyClean(p);
  dst[n++] = '\t';
  copyClean(t);
  dst[n++] = '\n';
  dst[n] = '\0';
  return n;
}

bool parseLine(const char* line, uint32_t& hash, const char** rest) {
  uint32_t h = 0;
  for (int i = 0; i < 8; i++) {
    const char c = line[i];
    uint32_t d;
    if (c >= '0' && c <= '9') {
      d = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      d = static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return false;  // also covers a line that ends early (NUL)
    }
    h = (h << 4) | d;
  }
  if (line[8] != '\t') return false;
  hash = h;
  *rest = line + 9;
  return true;
}

void resolveHits(std::vector<Hit>& hits) {
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    return a.hash != b.hash ? a.hash < b.hash : a.seq < b.seq;
  });
  size_t out = 0;
  for (size_t i = 0; i < hits.size(); i++) {
    if (i + 1 < hits.size() && hits[i + 1].hash == hits[i].hash) continue;  // a later line for this book wins
    if (hits[i].mask != 0) hits[out++] = hits[i];
  }
  hits.resize(out);
}

uint8_t maskFor(const std::vector<Hit>& hits, uint32_t hash) {
  const auto it = std::lower_bound(hits.begin(), hits.end(), hash,
                                   [](const Hit& h, uint32_t v) { return h.hash < v; });
  return (it != hits.end() && it->hash == hash) ? it->mask : 0;
}

}  // namespace ao3text
