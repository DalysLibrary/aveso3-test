#pragma once

#include <stddef.h>
#include <stdint.h>

struct CompactIndexRecord;

// Text search and first-letter filtering for the AO3 Library.
//
// Everything here works on fixed-size buffers and never allocates, so a search over the whole library
// costs a few hundred bytes of stack on top of the single pass the library already makes over its index
// file (see Ao3LibraryActivity::rebuildViewEntries).
//
// The folding idea (lower case, accents ignored) and the fold table come from VaultInk's search.
namespace ao3search {

constexpr size_t kMaxWords = 6;    // words in one search; extra words are ignored
constexpr size_t kWordBytes = 32;  // bytes kept per word (including the terminating NUL)

// Bucket for the "Title starts with" / "Author starts with" filter:
//   'A'..'Z'  the first letter, with accents ignored ("Élan" -> 'E', "Łódź" -> 'L')
//   '#'       digits, symbols and alphabets other than Latin (Greek, Cyrillic, CJK, ...)
//   0         empty text
// Leading spaces and ASCII punctuation (quotes, brackets, ...) are skipped.
char letterBucket(const char* text);

// Writes a lower-case, accent-free copy of the UTF-8 text `src` (at most `srcMax` bytes, or up to its
// terminating NUL) into `dst`, which always ends with a NUL. Returns the number of bytes written without the NUL.
size_t foldInto(char* dst, size_t cap, const char* src, size_t srcMax = static_cast<size_t>(-1));

// A search typed by the reader: words separated by spaces or punctuation. A book matches when every word is
// found somewhere in its title, author, series, fandom or relationships (case and accents ignored).
// A word with a '-' in front ("-angst") must NOT be found.
class Query {
 public:
  Query() { clear(); }

  void clear();
  void parse(const char* text);
  bool empty() const { return count_ == 0; }

  // `foldedHaystack` must already be folded (see foldInto).
  bool matches(const char* foldedHaystack) const;

 private:
  char words_[kMaxWords][kWordBytes];
  bool exclude_[kMaxWords];
  uint8_t count_;
};

// True when `query` matches the searchable text of one index record.
bool recordMatches(const Query& query, const CompactIndexRecord& rec);

}  // namespace ao3search
