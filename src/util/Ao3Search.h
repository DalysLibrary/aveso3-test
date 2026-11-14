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

constexpr size_t kMaxWords = 6;    // search terms (words and quoted phrases together); extra terms are ignored
constexpr size_t kWordBytes = 48;  // bytes kept per term (including the terminating NUL)

// Separates the fields of one searched text (title, author, series ...), so a quoted phrase never runs from one
// field into the next. Tag text and file path are separated by a TAB in the tag file, tags by commas.
constexpr char kFieldSeparator = '\x1f';

// Bucket for the "Title starts with" / "Author starts with" filter:
//   'A'..'Z'  the first letter, with accents ignored ("Élan" -> 'E', "Łódź" -> 'L')
//   '#'       digits, symbols and alphabets other than Latin (Greek, Cyrillic, CJK, ...)
//   0         empty text
// Leading spaces and ASCII punctuation (quotes, brackets, ...) are skipped.
char letterBucket(const char* text);

// Writes a lower-case, accent-free copy of the UTF-8 text `src` (at most `srcMax` bytes, or up to its
// terminating NUL) into `dst`, which always ends with a NUL. Returns the number of bytes written without the NUL.
size_t foldInto(char* dst, size_t cap, const char* src, size_t srcMax = static_cast<size_t>(-1));

// A search typed by the reader. Case and accents never matter.
//   fluff angst        a book matches when ANY of the words is found (anywhere in a word: "fluff" finds "Fluffy")
//   "omega ilya"       a phrase in quotation marks matches only that exact phrase: whole words, in that order,
//                      inside one tag (or one title, author ...). Text before or after it is fine. Spaces, hyphens and
//                      other punctuation between the words are interchangeable, so it finds "Omega Ilya Kuryakin",
//                      "Omega!Ilya" and "omega-ilya", but not "Omega, Ilya" (two tags) or "Omega Space Ilya".
//   -angst  -"slow burn"  a word or phrase with a minus in front must NOT be found
// Words and phrases can be mixed:  omega "hurt comfort" -angst
class Query {
 public:
  Query() { clear(); }

  void clear();
  void parse(const char* text);
  bool empty() const { return count_ == 0; }

  // `foldedHaystack` must already be folded (see foldInto).
  bool matches(const char* foldedHaystack) const;

  // Which of the terms occur in `foldedHaystack`: bit k is set when term k is found. A search can be answered from
  // several pieces of text (the index record, then the tag file) by OR-ing the masks together, then asking
  // satisfiedBy(). Terms with a minus in front are reported the same way (bit set = found).
  uint8_t foundMask(const char* foldedHaystack) const;
  bool satisfiedBy(uint8_t mask) const;

 private:
  char words_[kMaxWords][kWordBytes];  // a word, or a phrase as lower-case words separated by single spaces
  bool exclude_[kMaxWords];
  bool phrase_[kMaxWords];
  uint8_t count_;
};

// True when `query` matches the searchable text of one index record.
bool recordMatches(const Query& query, const CompactIndexRecord& rec);

// Same text as recordMatches() would search, reported as a word mask (see Query::foundMask).
uint8_t recordFoundMask(const Query& query, const CompactIndexRecord& rec);

}  // namespace ao3search
