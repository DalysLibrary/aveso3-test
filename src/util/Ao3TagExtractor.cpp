#include "Ao3TagExtractor.h"

#include <string.h>

namespace ao3text {
namespace {

// Labels whose lists are kept, in the form they appear on the preface page (before the colon).
// "Genre" is where FanFicFare epubs put the author's free-text tags.
constexpr const char* kLabels[] = {"Additional Tags", "Genre"};
constexpr int kLabelCount = sizeof(kLabels) / sizeof(kLabels[0]);
constexpr uint8_t kLabelGroup[kLabelCount] = {0, 1};

// Other labels found on AO3 and FanFicFare pages. A line that starts with one of these is not the value list of an
// empty label on the line before it.
constexpr const char* kOtherLabels[] = {"Rating",     "Archive Warning", "Archive Warnings", "Warning",   "Warnings",
                                        "Language",   "Stats",           "Series",           "Summary",   "Published",
                                        "Updated",    "Words",           "Chapters",         "Status",    "Title",
                                        "Author",     "Publisher",       "Packaged",         "Completed", "Category",
                                        "Fandom",     "Fandoms",         "Relationship",     "Relationships",
                                        "Character",  "Characters"};

constexpr const char* kBreakingTags[] = {"p",  "div", "li", "ul", "ol",   "dl",         "tr",   "td",
                                         "th", "table", "br", "hr", "blockquote", "h1", "h2",   "h3",
                                         "h4", "h5",  "h6", "body", "html", "head",     "section"};

bool isAlnum(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Does the tag with this name end a line of text? Opening <dt> does (a new label starts) and closing </dd> does
// (its value ends), but </dt> and <dd> do not, so "Label:" and its value stay on one line.
bool breaksLine(const char* name, bool closing) {
  for (const char* t : kBreakingTags) {
    if (strcmp(name, t) == 0) return true;
  }
  return closing ? strcmp(name, "dd") == 0 : strcmp(name, "dt") == 0;
}

uint32_t decodeEntity(const char* e, size_t n) {
  if (n == 0) return 0;
  if (e[0] == '#') {
    uint32_t cp = 0;
    size_t i = 1;
    uint32_t base = 10;
    if (n > 1 && (e[1] == 'x' || e[1] == 'X')) {
      base = 16;
      i = 2;
    }
    if (i >= n) return 0;
    for (; i < n; i++) {
      const char c = e[i];
      uint32_t d;
      if (c >= '0' && c <= '9') {
        d = static_cast<uint32_t>(c - '0');
      } else if (base == 16 && c >= 'a' && c <= 'f') {
        d = static_cast<uint32_t>(c - 'a' + 10);
      } else if (base == 16 && c >= 'A' && c <= 'F') {
        d = static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return 0;
      }
      cp = cp * base + d;
      if (cp > 0x10FFFF) return 0;
    }
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return cp == 0xA0 ? ' ' : cp;
  }
  struct Named {
    const char* name;
    uint32_t cp;
  };
  static const Named kNamed[] = {{"amp", '&'},      {"lt", '<'},        {"gt", '>'},        {"quot", '"'},
                                 {"apos", '\''},    {"nbsp", ' '},      {"ndash", 0x2013},  {"mdash", 0x2014},
                                 {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019},  {"ldquo", 0x201C},
                                 {"rdquo", 0x201D}};
  for (const Named& nm : kNamed) {
    if (strlen(nm.name) == n && memcmp(nm.name, e, n) == 0) return nm.cp;
  }
  return 0;
}

// If `p` (n bytes, no leading spaces) starts with one of the labels followed by a colon, returns the label's index
// and sets *after to the number of bytes up to and including the colon. Otherwise returns -1.
int matchLabel(const char* p, size_t n, size_t* after) {
  for (int i = 0; i < kLabelCount; i++) {
    const char* label = kLabels[i];
    const size_t ll = strlen(label);
    if (n < ll) continue;
    bool same = true;
    for (size_t k = 0; k < ll; k++) {
      if (lowerAscii(p[k]) != lowerAscii(label[k])) {
        same = false;
        break;
      }
    }
    if (!same) continue;
    size_t k = ll;
    while (k < n && p[k] == ' ') k++;
    if (k < n && p[k] == ':') {
      *after = k + 1;
      return i;
    }
  }
  return -1;
}

// Does the line start with one of the other labels ("Rating:", "Stats:", ...)?
bool startsWithOtherLabel(const char* p, size_t n) {
  for (const char* label : kOtherLabels) {
    const size_t ll = strlen(label);
    if (n < ll) continue;
    bool same = true;
    for (size_t k = 0; k < ll; k++) {
      if (lowerAscii(p[k]) != lowerAscii(label[k])) {
        same = false;
        break;
      }
    }
    if (!same) continue;
    size_t k = ll;
    while (k < n && p[k] == ' ') k++;
    if (k < n && p[k] == ':') return true;
  }
  return false;
}

}  // namespace

void TagExtractor::reset() {
  mode_ = Mode::Text;
  done_ = false;
  scanned_ = 0;
  lineLen_ = 0;
  line_[0] = '\0';
  tagName_[0] = '\0';
  tagLen_ = 0;
  tagBytes_ = 0;
  tagClosing_ = false;
  tagNameDone_ = false;
  quote_ = 0;
  entity_[0] = '\0';
  entityLen_ = 0;
  pendingLabel_ = -1;
  seenLabels_ = 0;
  out_[0] = '\0';
  outLen_ = 0;
}

bool TagExtractor::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (done_) return false;
    if (++scanned_ > kMaxScanBytes) {
      done_ = true;
      return false;
    }
    handleByte(data[i]);
  }
  return !done_;
}

void TagExtractor::finish() {
  if (mode_ == Mode::Entity) {
    appendText('&');
    for (uint8_t i = 0; i < entityLen_; i++) appendText(static_cast<uint8_t>(entity_[i]));
  }
  mode_ = Mode::Text;
  endLine();
}

void TagExtractor::startPage() {
  mode_ = Mode::Text;
  done_ = false;
  scanned_ = 0;
  lineLen_ = 0;
  pendingLabel_ = -1;
}

void TagExtractor::handleByte(uint8_t b) {
  switch (mode_) {
    case Mode::Text:
      if (b == '<') {
        mode_ = Mode::Tag;
        tagLen_ = 0;
        tagBytes_ = 0;
        tagClosing_ = false;
        tagNameDone_ = false;
        quote_ = 0;
        tagName_[0] = '\0';
      } else if (b == '&') {
        mode_ = Mode::Entity;
        entityLen_ = 0;
      } else {
        appendText(b);
      }
      break;
    case Mode::Tag:
      handleTagByte(b);
      break;
    case Mode::Entity:
      handleEntityByte(b);
      break;
  }
}

void TagExtractor::handleTagByte(uint8_t b) {
  // A '<' that never closes would swallow the rest of the page; give up on it after a while.
  if (++tagBytes_ > 1024) {
    mode_ = Mode::Text;
    return;
  }
  if (quote_ != 0) {
    if (static_cast<char>(b) == quote_) quote_ = 0;
    return;
  }
  if (b == '>') {
    endTag();
    return;
  }
  if (b == '"' || b == '\'') {
    quote_ = static_cast<char>(b);
    tagNameDone_ = true;
    return;
  }
  if (tagNameDone_) return;
  if (tagLen_ == 0 && b == '/') {
    tagClosing_ = true;
  } else if (isAlnum(b) && tagLen_ < sizeof(tagName_) - 1) {
    tagName_[tagLen_++] = lowerAscii(static_cast<char>(b));
    tagName_[tagLen_] = '\0';
  } else {
    tagNameDone_ = true;
  }
}

void TagExtractor::endTag() {
  mode_ = Mode::Text;
  if (tagLen_ > 0 && breaksLine(tagName_, tagClosing_)) {
    endLine();
  } else {
    appendText(' ');  // inline tags such as <a> and <b> never glue two words together
  }
}

void TagExtractor::handleEntityByte(uint8_t b) {
  if (b == ';') {
    endEntity();
    return;
  }
  if ((isAlnum(b) || b == '#') && entityLen_ < sizeof(entity_) - 1) {
    entity_[entityLen_++] = static_cast<char>(b);
    entity_[entityLen_] = '\0';
    return;
  }
  // Not an entity after all: keep the text as it was and look at this byte normally.
  appendText('&');
  for (uint8_t i = 0; i < entityLen_; i++) appendText(static_cast<uint8_t>(entity_[i]));
  mode_ = Mode::Text;
  handleByte(b);
}

void TagExtractor::endEntity() {
  mode_ = Mode::Text;
  const uint32_t cp = decodeEntity(entity_, entityLen_);
  if (cp == 0) {
    appendText('&');
    for (uint8_t i = 0; i < entityLen_; i++) appendText(static_cast<uint8_t>(entity_[i]));
    appendText(';');
  } else {
    appendCodepoint(cp);
  }
}

void TagExtractor::appendCodepoint(uint32_t cp) {
  if (cp < 0x80) {
    appendText(static_cast<uint8_t>(cp));
  } else if (cp < 0x800) {
    appendText(static_cast<uint8_t>(0xC0 | (cp >> 6)));
    appendText(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    appendText(static_cast<uint8_t>(0xE0 | (cp >> 12)));
    appendText(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
    appendText(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
  } else {
    appendText(static_cast<uint8_t>(0xF0 | (cp >> 18)));
    appendText(static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F)));
    appendText(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
    appendText(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
  }
}

// Adds one byte of page text to the current line. Runs of white space (and stray control bytes) become one space.
void TagExtractor::appendText(uint8_t b) {
  if (b == ' ' || b < 0x20) {
    if (lineLen_ > 0 && line_[lineLen_ - 1] != ' ' && lineLen_ < kLineBytes - 1) line_[lineLen_++] = ' ';
    return;
  }
  if (lineLen_ < kLineBytes - 1) line_[lineLen_++] = static_cast<char>(b);
}

// The current line of page text is complete: if it is a "Label: values" line, keep the values.
void TagExtractor::endLine() {
  size_t begin = 0;
  size_t end = lineLen_;
  while (begin < end && line_[begin] == ' ') begin++;
  while (end > begin && line_[end - 1] == ' ') end--;
  lineLen_ = 0;
  if (end == begin) return;
  line_[end] = '\0';  // end <= lineLen_ <= kLineBytes - 1, so this stays inside the buffer

  char* text = line_ + begin;
  const size_t n = end - begin;

  size_t after = 0;
  const int label = matchLabel(text, n, &after);
  if (label >= 0) {
    char* value = text + after;
    while (*value == ' ') value++;
    if (*value == '\0') {
      pendingLabel_ = label;  // the values are on the next line
    } else {
      pendingLabel_ = -1;
      storeValue(label, value);
    }
  } else if (pendingLabel_ >= 0) {
    const int pending = pendingLabel_;
    pendingLabel_ = -1;
    if (!startsWithOtherLabel(text, n)) storeValue(pending, text);
  }
  line_[0] = '\0';
}

// `value` points into line_, which is ours to change.
void TagExtractor::storeValue(int label, char* value) {
  const uint16_t bit = static_cast<uint16_t>(1u << kLabelGroup[label]);
  if (seenLabels_ & bit) return;  // the first list of each kind wins
  seenLabels_ |= bit;

  size_t len = strlen(value);
  if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {  // some epubs quote the whole list
    value++;
    len -= 2;
    value[len] = '\0';
  }
  if (len > kValueBytes - 1) {
    len = kValueBytes - 1;
    while (len > 0 && (static_cast<uint8_t>(value[len]) & 0xC0) == 0x80) len--;  // do not cut a character in half
  }
  while (len > 0 && value[len - 1] == ' ') len--;
  if (len == 0) return;

  size_t room = kTextBytes - 1 - outLen_;
  if (outLen_ > 0) {
    if (room < 3) return;
    out_[outLen_++] = ';';
    out_[outLen_++] = ' ';
    room -= 2;
  }
  if (len > room) {
    len = room;
    while (len > 0 && (static_cast<uint8_t>(value[len]) & 0xC0) == 0x80) len--;
  }
  // Copy, dropping the space that inline tags leave in front of a comma ("Fluff , Angst" -> "Fluff, Angst").
  for (size_t i = 0; i < len; i++) {
    if (value[i] == ' ' && i + 1 < len && (value[i + 1] == ',' || value[i + 1] == ';')) continue;
    out_[outLen_++] = value[i];
  }
  out_[outLen_] = '\0';
}

}  // namespace ao3text
