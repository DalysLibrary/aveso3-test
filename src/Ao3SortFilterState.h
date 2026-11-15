#pragma once
#include <stdint.h>
#include <string.h>

enum class SortMode : uint8_t {
    ALPHABETIC  = 0,
    WORD_COUNT  = 1,
    DATE_ADDED  = 2,
    SERIES      = 3,
    AUTHOR      = 4
};

// How the status, publication and favourite groups combine.
// Inside a group a book may match any ticked row. ALL (the default) requires
// every group that has a tick. ANY lets a book match any such group.
enum class CombineMode : uint8_t {
    ALL = 0,
    ANY = 1
};

struct SortFilterState {
    char     fandom[32] = {};
    char     relationship[32] = {};
    bool     relationshipNoneOnly = false;
    SortMode sortMode = SortMode::ALPHABETIC;
    bool     ascending = true;
    uint64_t titleInitials = 0;
    uint64_t authorInitials = 0;
    uint8_t  statusMask = 0;        // bits 0-4: Unread, Reading, Finished, Waiting, New Chapter
    uint8_t  publicationMask = 0;   // bit 0 WIP, bit 1 Completed in AO3
    bool     favouriteOnly = false;
    CombineMode combine = CombineMode::ALL;
    char     textQuery[32] = {};
};

inline bool filterFieldsDiffer(const SortFilterState& a, const SortFilterState& b) {
    return strcmp(a.fandom, b.fandom) != 0 || strcmp(a.relationship, b.relationship) != 0 ||
           a.relationshipNoneOnly != b.relationshipNoneOnly || a.titleInitials != b.titleInitials ||
           a.authorInitials != b.authorInitials || a.statusMask != b.statusMask ||
           a.publicationMask != b.publicationMask || a.favouriteOnly != b.favouriteOnly || a.combine != b.combine ||
           strcmp(a.textQuery, b.textQuery) != 0;
}
