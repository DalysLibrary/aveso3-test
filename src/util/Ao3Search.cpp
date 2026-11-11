#include "Ao3Search.h"

#include <string.h>

#include <algorithm>
#include <iterator>

#include "../Ao3CompactIndexRecord.h"
#include "FoldTable.h"

namespace ao3search {
namespace {

// Decodes the UTF-8 character starting at s[i] (n = usable bytes) and moves i past it.
// Returns false (and leaves i at n) when the text ends in the middle of a character.
// A stray byte that is not valid UTF-8 is returned as is, one byte at a time.
bool decodeAt(const unsigned char* s, size_t n, size_t& i, uint32_t& cp) {
  const unsigned char b = s[i];
  size_t len = 1;
  if (b >= 0xF0 && b < 0xF8)
    len = 4;
  else if (b >= 0xE0)
    len = (b < 0xF0) ? 3 : 1;
  else if (b >= 0xC0)
    len = 2;

  if (len > 1) {
    if (i + len > n) {
      i = n;
      return false;
    }
    bool valid = true;
    for (size_t k = 1; k < len; k++) {
      if ((s[i + k] & 0xC0) != 0x80) valid = false;
    }
    if (!valid) len = 1;
  }

  if (len == 1) {
    cp = b;
  } else if (len == 2) {
    cp = b & 0x1Fu;
  } else if (len == 3) {
    cp = b & 0x0Fu;
  } else {
    cp = b & 0x07u;
  }
  for (size_t k = 1; k < len; k++) cp = (cp << 6) | (s[i + k] & 0x3Fu);
  i += len;
  return true;
}

// Number of bytes of a code point in UTF-8.
size_t encodedSize(uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

void encode(uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Lower case with accents removed. Returns 0 for a combining accent, which is dropped.
uint32_t foldCodepoint(uint32_t cp) {
  if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + ('a' - 'A') : cp;
  if (cp >= 0x0300 && cp <= 0x036F) return 0;  // combining accent
  // Letters whose "accent" is part of the shape, which VaultInk's table keeps apart: fold them to the
  // plain letter so "lodz" finds "Łódź" and "Ørsted" is found under O.
  switch (cp) {
    case 0x0141:
    case 0x0142:
      return 'l';
    case 0x00D8:
    case 0x00F8:
      return 'o';
    case 0x0110:
    case 0x0111:
      return 'd';
    case 0x0126:
    case 0x0127:
      return 'h';
    case 0x0131:
      return 'i';
    default:
      break;
  }
  if (cp < 0xC0 || cp > 0xFFFF) return cp;
  const auto* begin = std::begin(foldtable::PAIRS);
  const auto* end = std::end(foldtable::PAIRS);
  const auto* it = std::lower_bound(begin, end, cp, [](const foldtable::Pair& p, uint32_t v) { return p.from < v; });
  return (it != end && it->from == cp) ? it->to : cp;
}

// ASCII character that separates words (everything except letters, digits and the apostrophe).
// Bytes of non-ASCII characters (accented, Greek, Cyrillic, ...) count as part of a word.
bool isSeparator(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  if (u >= 0x80) return false;
  return !((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '\'');
}

}  // namespace

size_t foldInto(char* dst, size_t cap, const char* src, size_t srcMax) {
  if (!dst || cap == 0) return 0;
  size_t out = 0;
  if (src) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(src);
    size_t n = 0;
    while (n < srcMax && s[n]) n++;
    size_t i = 0;
    while (i < n) {
      uint32_t cp;
      if (!decodeAt(s, n, i, cp)) break;
      cp = foldCodepoint(cp);
      if (cp == 0) continue;
      const size_t need = encodedSize(cp);
      if (out + need >= cap) break;  // keep room for the NUL
      encode(cp, dst + out);
      out += need;
    }
  }
  dst[out] = '\0';
  return out;
}

char letterBucket(const char* text) {
  if (!text) return 0;
  const unsigned char* s = reinterpret_cast<const unsigned char*>(text);
  size_t n = 0;
  while (s[n] && n < 16) n++;  // the first letter is always within the first few bytes
  size_t i = 0;
  while (i < n) {
    uint32_t cp;
    if (!decodeAt(s, n, i, cp)) break;
    cp = foldCodepoint(cp);
    if (cp == 0) continue;  // a stray combining accent
    if (cp < 0x80) {
      if (cp >= 'a' && cp <= 'z') return static_cast<char>(cp - ('a' - 'A'));
      if (cp >= '0' && cp <= '9') return '#';
      continue;  // space or punctuation in front of the title: look at the next character
    }
    return '#';
  }
  return 0;
}

void Query::clear() {
  count_ = 0;
  for (size_t k = 0; k < kMaxWords; k++) {
    words_[k][0] = '\0';
    exclude_[k] = false;
  }
}

void Query::parse(const char* text) {
  clear();
  if (!text) return;

  char folded[128];
  const size_t n = foldInto(folded, sizeof(folded), text);

  size_t i = 0;
  while (i < n && count_ < kMaxWords) {
    while (i < n && isSeparator(folded[i])) i++;
    if (i >= n) break;

    const size_t start = i;
    while (i < n && !isSeparator(folded[i])) i++;

    // "-word" (a minus at the start of the search or after a space) excludes the word.
    const bool exclude = start >= 1 && folded[start - 1] == '-' &&
                         (start == 1 || folded[start - 2] == ' ' || folded[start - 2] == '\t');

    size_t len = i - start;
    if (len > kWordBytes - 1) {
      len = kWordBytes - 1;
      // Do not cut a character in half.
      while (len > 0 && (static_cast<unsigned char>(folded[start + len]) & 0xC0) == 0x80) len--;
    }
    if (len == 0) continue;

    memcpy(words_[count_], folded + start, len);
    words_[count_][len] = '\0';
    exclude_[count_] = exclude;
    count_++;
  }
}

bool Query::matches(const char* foldedHaystack) const {
  if (!foldedHaystack) return count_ == 0;
  for (uint8_t k = 0; k < count_; k++) {
    const bool found = strstr(foldedHaystack, words_[k]) != nullptr;
    if (found == exclude_[k]) return false;  // wanted word missing, or excluded word present
  }
  return true;
}

bool recordMatches(const Query& query, const CompactIndexRecord& rec) {
  if (query.empty()) return true;

  char hay[320];
  size_t used = 0;
  const auto add = [&](const char* field, size_t fieldBytes) {
    if (field[0] == '\0' || used + 2 >= sizeof(hay)) return;
    used += foldInto(hay + used, sizeof(hay) - used, field, fieldBytes);
    if (used + 1 < sizeof(hay)) {
      hay[used++] = ' ';
      hay[used] = '\0';
    }
  };
  hay[0] = '\0';
  add(rec.title, sizeof(rec.title));
  add(rec.author, sizeof(rec.author));
  add(rec.seriesName, sizeof(rec.seriesName));
  add(rec.fandom, sizeof(rec.fandom));
  add(rec.relationship1, sizeof(rec.relationship1));
  add(rec.relationship2, sizeof(rec.relationship2));
  return query.matches(hay);
}

}  // namespace ao3search
