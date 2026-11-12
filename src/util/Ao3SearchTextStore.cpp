#include <Arduino.h>
#include <HalStorage.h>

#include <algorithm>
#include <memory>

#include "Ao3SearchText.h"

namespace ao3text {

bool appendLine(uint32_t hash, const char* path, const char* text) {
  char line[kMaxLineBytes];
  const size_t n = formatLine(line, sizeof(line), hash, path, text);
  if (n == 0) return false;

  if (!Storage.exists(kSearchTextPath)) {
    HalFile created;
    if (!Storage.openFileForWrite("AO3T", kSearchTextPath, created)) return false;
    created.close();
  }
  HalFile f = Storage.open(kSearchTextPath, O_RDWR);
  if (!f) return false;
  f.seek(f.size());
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(line), n);
  f.close();
  return written == n;
}

void loadHashes(std::vector<uint32_t>& out) {
  out.clear();
  if (!Storage.exists(kSearchTextPath)) return;
  HalFile f;
  if (!Storage.openFileForRead("AO3T", kSearchTextPath, f)) return;

  auto splitter = std::unique_ptr<LineSplitter>(new LineSplitter());
  const auto onLine = [&out](const char* line, size_t) {
    uint32_t hash;
    const char* rest;
    if (parseLine(line, hash, &rest)) out.push_back(hash);
  };

  char chunk[512];
  int got;
  int rounds = 0;
  while ((got = f.read(chunk, sizeof(chunk))) > 0) {
    splitter->feed(chunk, static_cast<size_t>(got), onLine);
    if ((++rounds & 7) == 0) yield();
  }
  splitter->finish(onLine);
  f.close();

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

void collectHits(const ao3search::Query& query, std::vector<Hit>& hits) {
  hits.clear();
  if (query.empty() || !Storage.exists(kSearchTextPath)) return;
  HalFile f;
  if (!Storage.openFileForRead("AO3T", kSearchTextPath, f)) return;

  struct Work {
    LineSplitter splitter;
    char folded[kMaxLineBytes];
  };
  auto work = std::unique_ptr<Work>(new Work());
  uint16_t seq = 0;

  const auto onLine = [&](const char* line, size_t) {
    uint32_t hash;
    const char* rest;
    if (!parseLine(line, hash, &rest)) return;
    if (hits.size() >= kMaxHits) {
      resolveHits(hits);  // squeeze out old duplicates; the newest line of each book is kept
      if (hits.size() >= kMaxHits) return;
    }
    ao3search::foldInto(work->folded, sizeof(work->folded), rest);
    Hit hit;
    hit.hash = hash;
    hit.seq = seq++;
    hit.mask = query.foundMask(work->folded);
    hits.push_back(hit);
  };

  char chunk[512];
  int got;
  int rounds = 0;
  while ((got = f.read(chunk, sizeof(chunk))) > 0) {
    work->splitter.feed(chunk, static_cast<size_t>(got), onLine);
    if ((++rounds & 7) == 0) yield();
  }
  work->splitter.finish(onLine);
  f.close();

  resolveHits(hits);
}

}  // namespace ao3text
