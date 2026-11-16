#pragma once

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "Ao3CompactIndexRecord.h"

// Index flags. Bit 0 and bit 1 are the existing tombstone and finished marks.
// Bits 4-6 hold the reading status (BookStatus 0-5). Bit 7 is unused.
constexpr uint8_t kIndexTombstone = 0x01;
constexpr uint8_t kIndexFinished = 0x02;
constexpr uint8_t kIndexFavourite = 0x04;
constexpr uint8_t kIndexMetaCompleted = 0x08;
constexpr uint8_t kIndexStatusShift = 4;
constexpr uint8_t kIndexStatusMask = 0x70;

inline uint8_t indexFlagsWithStatus(uint8_t flags, uint8_t status) {
  return static_cast<uint8_t>((flags & ~kIndexStatusMask) | ((status & 0x07) << kIndexStatusShift));
}

inline uint8_t statusFromIndexFlags(uint8_t flags) {
  return static_cast<uint8_t>((flags & kIndexStatusMask) >> kIndexStatusShift);
}

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

// Accented Latin letters fold onto A-Z. Anything else, including a leading
// quote, dash or bracket, stays on '&'.
inline char foldLatin(uint32_t cp) {
  if ((cp >= 0x00C0 && cp <= 0x00C5) || cp == 0x00C6 || (cp >= 0x00E0 && cp <= 0x00E5) || cp == 0x00E6 ||
      (cp >= 0x0100 && cp <= 0x0105))
    return 'A';
  if (cp == 0x00C7 || cp == 0x00E7 || (cp >= 0x0106 && cp <= 0x010D)) return 'C';
  if (cp == 0x00D0 || cp == 0x00F0 || (cp >= 0x010E && cp <= 0x0111)) return 'D';
  if ((cp >= 0x00C8 && cp <= 0x00CB) || (cp >= 0x00E8 && cp <= 0x00EB) || (cp >= 0x0112 && cp <= 0x011B)) return 'E';
  if (cp >= 0x011C && cp <= 0x0123) return 'G';
  if ((cp >= 0x00CC && cp <= 0x00CF) || (cp >= 0x00EC && cp <= 0x00EF) || (cp >= 0x0128 && cp <= 0x0131) ||
      cp == 0x0132 || cp == 0x0133)
    return 'I';
  if ((cp >= 0x0139 && cp <= 0x013E) || (cp >= 0x013F && cp <= 0x0142)) return 'L';
  if (cp == 0x00D1 || cp == 0x00F1 || (cp >= 0x0143 && cp <= 0x0148)) return 'N';
  if ((cp >= 0x00D2 && cp <= 0x00D6) || cp == 0x00D8 || (cp >= 0x00F2 && cp <= 0x00F6) || cp == 0x00F8 ||
      (cp >= 0x014C && cp <= 0x0153))
    return 'O';
  if (cp >= 0x0154 && cp <= 0x0159) return 'R';
  if (cp == 0x00DF || (cp >= 0x015A && cp <= 0x0161)) return 'S';
  if (cp >= 0x0162 && cp <= 0x0167) return 'T';
  if ((cp >= 0x00D9 && cp <= 0x00DC) || (cp >= 0x00F9 && cp <= 0x00FC) || (cp >= 0x0168 && cp <= 0x0173)) return 'U';
  if (cp == 0x00DD || cp == 0x00FD || cp == 0x00FF || cp == 0x0178 || cp == 0x0176 || cp == 0x0177) return 'Y';
  if (cp >= 0x0179 && cp <= 0x017E) return 'Z';
  return 0;
}

inline int initialBitOf(const char* s) {
  if (!s || s[0] == '\0') return -1;
  const unsigned char c0 = static_cast<unsigned char>(s[0]);
  if (c0 < 0x80) return initialBit(c0);
  if ((c0 & 0xE0) == 0xC0 && (static_cast<unsigned char>(s[1]) & 0xC0) == 0x80) {
    const uint32_t cp = (uint32_t(c0 & 0x1F) << 6) | (static_cast<unsigned char>(s[1]) & 0x3F);
    const char base = foldLatin(cp);
    if (base >= 'A' && base <= 'Z') return 10 + (base - 'A');
    return kInitialOtherBit;
  }
  return kInitialOtherBit;
}

inline bool initialPasses(uint64_t mask, const char* s) {
  if (mask == 0) return true;
  const int bit = initialBitOf(s);
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

inline bool containsFold(const char* hay, size_t hayLen, const char* needle) {
  if (!needle || needle[0] == '\0') return true;
  if (!hay || hayLen == 0) return false;
  const size_t nlen = strlen(needle);
  if (nlen == 0 || nlen > hayLen) return false;
  for (size_t start = 0; start + nlen <= hayLen && hay[start] != '\0'; start++) {
    size_t i = 0;
    while (i < nlen && start + i < hayLen && hay[start + i] != '\0' &&
           tolower(static_cast<unsigned char>(hay[start + i])) ==
               tolower(static_cast<unsigned char>(needle[i]))) {
      i++;
    }
    if (i == nlen) return true;
  }
  return false;
}

// Every word must appear in the index row. The row is already in memory, so
// this does not open a file per book. Opening the tag sidecar here locked the
// reader: the home button cannot run until the scan returns.
inline bool compactTextMatch(const CompactIndexRecord& rec, const char* query) {
  if (!query || query[0] == '\0') return true;
  const char* p = query;
  bool any = false;
  while (*p) {
    while (*p == ' ') p++;
    if (*p == '\0') break;
    const char* start = p;
    while (*p && *p != ' ') p++;
    char word[32];
    size_t n = static_cast<size_t>(p - start);
    if (n >= sizeof(word)) n = sizeof(word) - 1;
    memcpy(word, start, n);
    word[n] = '\0';
    any = true;
    const bool hit = containsFold(rec.title, sizeof(rec.title), word) ||
                     containsFold(rec.author, sizeof(rec.author), word) ||
                     containsFold(rec.seriesName, sizeof(rec.seriesName), word) ||
                     containsFold(rec.fandom, sizeof(rec.fandom), word) ||
                     containsFold(rec.relationship1, sizeof(rec.relationship1), word) ||
                     containsFold(rec.relationship2, sizeof(rec.relationship2), word);
    if (!hit) return false;
  }
  return true;
}
