#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ao3text {

constexpr size_t kLineBytes = 1536;     // longest line of page text that is looked at (long tag lists)
constexpr size_t kTextBytes = 720;      // longest result (all lists together)
constexpr size_t kValueBytes = 320;     // longest single list (e.g. all additional tags)
constexpr size_t kMaxScanBytes = 16384;  // stop reading a page after this many bytes
constexpr int kMaxTagWords = 7;          // a free-text tag with more words than this is left out (see storeValue)

// Pulls the full tag lists (fandoms, relationships, characters, additional tags, genre, category) out of the
// preface page of an AO3 fic, which is streamed through feed() in pieces of any size.
//
// The library's own scraper (Ao3Librarian) keeps only 4 short tags per book. This reads the same page again but
// keeps everything, for the text search. It works on the page as plain text: HTML tags are removed, block-level
// tags (<br>, <p>, <div>, </dd>, <dt> ...) end a line, and a line that starts with a label such as
// "Additional Tags:" gives the list for that label. That fits both AO3's own download and FanFicFare epubs.
//
// Uses fixed buffers and never allocates, so a failure on an unexpected page just means less (or no) text.
class TagExtractor {
 public:
  TagExtractor() { reset(); }

  void reset();

  // Feeds more of the page. Returns false once nothing more is needed (the caller may stop reading).
  bool feed(const uint8_t* data, size_t len);

  // Call after the last feed() so a final line without a closing tag is not lost.
  void finish();

  // Starts the next page of the same book: the scan limit and the half-read line are reset, but lists already
  // found are kept (and the first list of each kind still wins).
  void startPage();

  bool done() const { return done_; }
  bool hasText() const { return outLen_ > 0; }

  // The lists found, separated by "; ", e.g. "Supernatural; Castiel/Dean Winchester; Fluff, Angst".
  const char* text() const { return out_; }

 private:
  enum class Mode : uint8_t { Text, Tag, Entity };

  void handleByte(uint8_t b);
  void handleTagByte(uint8_t b);
  void handleEntityByte(uint8_t b);
  void endTag();
  void endEntity();
  void appendText(uint8_t b);
  void appendCodepoint(uint32_t cp);
  void endLine();
  void storeValue(int label, char* value);

  Mode mode_;
  bool done_;
  uint32_t scanned_;

  char line_[kLineBytes];
  size_t lineLen_;

  char tagName_[12];
  uint8_t tagLen_;
  uint16_t tagBytes_;
  bool tagClosing_;
  bool tagNameDone_;
  char quote_;

  char entity_[12];
  uint8_t entityLen_;

  int pendingLabel_;  // a label seen on its own line, waiting for its value on the next line
  uint16_t seenLabels_;

  char out_[kTextBytes];
  size_t outLen_;
};

}  // namespace ao3text
