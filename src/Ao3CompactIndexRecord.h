#pragma once
#include <stdint.h>

#pragma pack(push, 1)
struct CompactIndexRecord {
    char     title[64];         // truncated title
    char     author[32];        // truncated author
    uint32_t wordCount;         // from scraping
    uint32_t addedSequence;     // monotonically increasing counter assigned at index write time
    char     seriesName[32];    // from scraping
    uint16_t seriesPart;        // position within series, 0 if none
    char     fandom[32];        // scraped from epub HTML
    char     relationship1[32]; // primary pairing — scraped from epub HTML
    char     relationship2[32]; // secondary pairing — scraped from epub HTML
    uint32_t cacheHash;         // uint32_t cast of std::hash<std::string>{}(epubPath)
    uint8_t  flags;             // bit 0 = tombstone (deleted)
};
#pragma pack(pop)

// Exactly 239 bytes on disk
static_assert(sizeof(CompactIndexRecord) == 239, "CompactIndexRecord must be exactly 239 bytes");

// How many books the AO3 library can hold. The in-memory list of books takes 46 bytes per book (115 KB at 2500), which
// only devices with PSRAM can spare, so the higher limit is only for builds that have it (X4 Pro, X4 Classic, Paper
// Mono: the ones that define BOARD_HAS_PSRAM). The X3, the original X4 and every other build keep 1000.
#ifdef BOARD_HAS_PSRAM
constexpr uint16_t MAX_LIBRARY_BOOKS = 2500;
#else
constexpr uint16_t MAX_LIBRARY_BOOKS = 1000;
#endif
constexpr uint32_t INDEX_HEADER_SIZE = 12;

inline uint32_t offsetOf(uint16_t i) {
    return INDEX_HEADER_SIZE + i * (uint32_t)sizeof(CompactIndexRecord);
}
