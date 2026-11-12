#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "Ao3Search.h"

// The "search text" file: one line per AO3 book holding what the library index does not keep -- the book's file
// path (so folder and file names can be searched) and its additional tags (see Ao3TagExtractor).
//
// Line format:   <8 hex digits of the book's hash> TAB <file path> TAB <tag text> NEWLINE
//
// A search reads this file once from start to finish in small pieces and keeps only a few bytes per book, so the
// file can be larger than the free RAM.
namespace ao3text {

constexpr const char* kSearchTextPath = "/.crosspoint/ao3_tag_text.tsv";
constexpr size_t kMaxLineBytes = 900;   // longer lines are ignored (path up to 255 + tags up to 480 fit easily)
constexpr size_t kMaxHits = 2000;       // lines kept in memory at once during one search

// ---- parts that need no storage (also unit tested on a PC) ----------------------------------------------------

// Writes one line (including the NEWLINE) into dst. Tabs and line breaks inside path/text become spaces.
// Returns the number of bytes written, or 0 if it does not fit.
size_t formatLine(char* dst, size_t cap, uint32_t hash, const char* path, const char* text);

// Reads the hash at the start of a line; *rest points at the path (the part that is searched).
bool parseLine(const char* line, uint32_t& hash, const char** rest);

// Cuts a stream of bytes into lines without needing the whole file in memory. Lines longer than
// kMaxLineBytes are skipped.
class LineSplitter {
 public:
  template <class F>
  void feed(const char* data, size_t n, F&& onLine) {
    for (size_t i = 0; i < n; i++) {
      const char c = data[i];
      if (c == '\n') {
        endLine(onLine);
      } else if (c == '\r') {
        continue;
      } else if (len_ < sizeof(buf_) - 1) {
        buf_[len_++] = c;
      } else {
        tooLong_ = true;
      }
    }
  }

  template <class F>
  void finish(F&& onLine) {
    endLine(onLine);
  }

 private:
  template <class F>
  void endLine(F&& onLine) {
    if (!tooLong_ && len_ > 0) {
      buf_[len_] = '\0';
      onLine(static_cast<const char*>(buf_), len_);
    }
    len_ = 0;
    tooLong_ = false;
  }

  char buf_[kMaxLineBytes] = {};
  size_t len_ = 0;
  bool tooLong_ = false;
};

// What one search found in one line: which of the search words occur there.
struct Hit {
  uint32_t hash;
  uint16_t seq;  // line number; a later line for the same book replaces an earlier one
  uint8_t mask;
};

// Sorts by book, lets the last line of each book win, and drops books where no word was found.
void resolveHits(std::vector<Hit>& hits);

// Word mask for one book (0 if it has no line or no word was found). `hits` must have been through resolveHits().
uint8_t maskFor(const std::vector<Hit>& hits, uint32_t hash);

// ---- parts that read and write the SD card (Ao3SearchTextStore.cpp) -------------------------------------------

// Adds a line for a book. The file is created if needed.
bool appendLine(uint32_t hash, const char* path, const char* text);

// Hashes of all books that have a line, sorted.
void loadHashes(std::vector<uint32_t>& out);

// Reads the file once and fills `hits` for `query` (already resolved). A missing file is not an error: no hits.
void collectHits(const ao3search::Query& query, std::vector<Hit>& hits);

}  // namespace ao3text
