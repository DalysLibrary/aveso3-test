#pragma once

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "Ao3CompactIndexRecord.h"

// Index flags. Bit 0 and bit 1 are the existing tombstone and finished marks.
constexpr uint8_t kIndexTombstone = 0x01;
constexpr uint8_t kIndexFinished = 0x02;
constexpr uint8_t kIndexFavourite = 0x04;
constexpr uint8_t kIndexMetaCompleted = 0x08;

// ViewEntry::filterBits
constexpr uint8_t kFilterStatusMask = 0x07;
constexpr uint8_t kFilterFavourite = 0x08;
constexpr uint8_t kFilterMetaCompleted = 0x10;

// Initial-key bitmask: bits 0-9 digits, 10-35 letters, 36 = '&' (anything else).
constexpr int kInitialOtherBit = 36;

inline int initialBit(unsigned char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 32);
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  if (c == 0) return -1;
  return kInitialOtherBit;
}

inline bool initialPasses(uint64_t mask, unsigned char c) {
  if (mask == 0) return true;
  const int bit = initialBit(c);
  if (bit < 0) return false;
  return (mask & (uint64_t{1} << bit)) != 0;
}

inline void formatInitialMask(uint64_t mask, char* out, size_t outLen) {
  if (outLen == 0) return;
  if (mask == 0) {
    strncpy(out, "Any", outLen);
    out[outLen - 1] = '\0';
    return;
  }
  size_t n = 0;
  auto push = [&](char ch) {
    if (n + 2 >= outLen) return;
    if (n > 0 && n + 3 < outLen) out[n++] = ' ';
    out[n++] = ch;
  };
  for (int d = 0; d <= 9; d++)
    if (mask & (uint64_t{1} << d)) push(static_cast<char>('0' + d));
  for (int l = 0; l < 26; l++)
    if (mask & (uint64_t{1} << (10 + l))) push(static_cast<char>('A' + l));
  if (mask & (uint64_t{1} << kInitialOtherBit)) push('&');
  if (n + 1 < outLen) {
    out[n] = '\0';
  } else {
    out[outLen - 1] = '\0';
  }
  if (strlen(out) > 18) {
    out[15] = '.';
    out[16] = '.';
    out[17] = '\0';
  }
}

inline bool containsFold(const char* hay, const char* needle) {
  if (!needle || needle[0] == '\0') return true;
  if (!hay) return false;
  const size_t nlen = strlen(needle);
  for (const char* p = hay; *p; p++) {
    size_t i = 0;
    while (i < nlen && p[i] &&
           tolower(static_cast<unsigned char>(p[i])) == tolower(static_cast<unsigned char>(needle[i]))) {
      i++;
    }
    if (i == nlen) return true;
  }
  return false;
}

// Title, author, fandom and the two relationships live on the index row.
// The four short tags live only in the sidecar and are checked by the caller.
inline bool compactTextMatch(const CompactIndexRecord& rec, const char* query) {
  if (!query || query[0] == '\0') return true;
  return containsFold(rec.title, query) || containsFold(rec.author, query) || containsFold(rec.seriesName, query) ||
         containsFold(rec.fandom, query) || containsFold(rec.relationship1, query) ||
         containsFold(rec.relationship2, query);
}
