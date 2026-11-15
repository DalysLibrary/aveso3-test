#include <functional>
#include "Ao3LibraryActivity.h"
#include "../../Ao3TagMergeStore.h"
#include "../../Ao3Librarian.h"
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <HalDisplay.h>
#include <Serialization.h>
#include <I18n.h>
#include <cstring>
#include <algorithm>
#include <new>
#include <cstdlib>
#include <ArduinoJson.h>
#include "BookActionActivity.h"
#include "Ao3IndexActivity.h"
#include "Ao3LibrarySettingsActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "../../fontIds.h"
#include "../../MappedInputManager.h"
#include "../../components/UITheme.h"
#include "../../RecentBooksStore.h"
#include "../../CrossPointState.h"
// Silent restart to Dashboard or AO3 Library, depending on the hint set by the activity that called silentRestartToHint()
#include "SilentRestart.h"
#include "Ao3MarkedForLaterStore.h"

#include <FreeInkUIGfxRenderer.h>

namespace {
// Distance above the screen bottom for the bottom-right arrow when there is no
// button-hint bar (touch builds report buttonHintsHeight == 0).
constexpr int kBottomArrowRaise = 28;

enum FilterRow : int {
  FR_CLEAR_ALL = 0,
  FR_FANDOM = 1,
  FR_CLEAR_FANDOM = 2,
  FR_REL = 3,
  FR_CLEAR_REL = 4,
  FR_TITLE = 5,
  FR_CLEAR_TITLE = 6,
  FR_AUTHOR = 7,
  FR_CLEAR_AUTHOR = 8,
  FR_STATUS = 9,
  FR_CLEAR_STATUS = 10,
  FR_TEXT = 11,
  FR_CLEAR_TEXT = 12,
  FR_SORT = 13,
  FR_ORDER = 14,
  FR_APPLY = 15,
  FR_COUNT = 16
};

constexpr int kStatusRows = 10;

inline int bottomArrowBaseY(const int screenHeight, const int hintsHeight) {
  return screenHeight - hintsHeight - (hintsHeight > 0 ? 0 : kBottomArrowRaise);
}

int letterRowCount(int row) {
  static const int lens[] = {10, 10, 9, 8};
  return (row >= 0 && row < 4) ? lens[row] : 0;
}

char letterAt(int row, int col) {
  static const char* rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM&"};
  if (row < 0 || row > 3 || col < 0 || col >= letterRowCount(row)) return 0;
  return rows[row][col];
}

struct KeyBox {
  int x, y, w, h;
  char ch;
  bool clear;
  bool apply;
};

int layoutLetterKeys(int screenW, int originY, KeyBox* out, int cap) {
  int keyW = (screenW - 32 - 6 * 9) / 10;
  if (keyW > 52) keyW = 52;
  if (keyW < 28) keyW = 28;
  const int keyH = keyW > 40 ? 40 : 32;
  const int gap = 6;
  int n = 0;
  for (int row = 0; row < 4; row++) {
    const int cols = letterRowCount(row);
    const int total = cols * keyW + (cols - 1) * gap;
    int x = (screenW - total) / 2;
    const int y = originY + row * (keyH + gap);
    for (int col = 0; col < cols && n < cap; col++) {
      out[n++] = KeyBox{x, y, keyW, keyH, letterAt(row, col), false, false};
      x += keyW + gap;
    }
  }
  const int btnW = keyW * 3;
  const int btnY = originY + 4 * (keyH + gap) + 8;
  const int pair = btnW * 2 + gap * 2;
  const int bx = (screenW - pair) / 2;
  if (n < cap) out[n++] = KeyBox{bx, btnY, btnW, keyH, 0, true, false};
  if (n < cap) out[n++] = KeyBox{bx + btnW + gap * 2, btnY, btnW, keyH, 0, false, true};
  return n;
}

// fillRect's last argument is a bool: true draws black, false draws white.
// Passing the Color enum White is nonzero, so it painted the sheet black.
void strokeBox(GfxRenderer& renderer, int x, int y, int w, int h) {
  renderer.fillRect(x, y, w, 1, true);
  renderer.fillRect(x, y + h - 1, w, 1, true);
  renderer.fillRect(x, y, 1, h, true);
  renderer.fillRect(x + w - 1, y, 1, h, true);
}

void drawTickBox(GfxRenderer& renderer, int x, int y, bool on) {
  strokeBox(renderer, x, y, 16, 16);
  if (on) renderer.fillRect(x + 3, y + 3, 10, 10, true);
}

void highlightRow(GfxRenderer& renderer, int x, int y, int w, int h) {
  renderer.fillRoundedRect(x, y, w, h, 4, true, true, true, true, LightGray);
}
}  // namespace
// ---------------------------------------------------------------------------
//  onEnter
// ---------------------------------------------------------------------------

// Initialize static member to false on startup
bool Ao3LibraryActivity::pendingTransferScan = false;

void Ao3LibraryActivity::onEnter() {
  buttonNavigator.setMappedInputManager(mappedInput);
  indexState = IndexState::UNKNOWN;
  screenState = ScreenState::LIBRARY;
  selectorIndex = initialSelectorIndex_;   // restore position when returning from reader
  
  // If the user arrived here by holding BACK in the reader, 
  // the button is still physically pressed. We must ignore the subsequent release.
  skipNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  autoIndexLaunched_ = false;

  if (Storage.exists("/.crosspoint/pending_ao3_scan")) {
    Storage.remove("/.crosspoint/pending_ao3_scan");
    pendingTransferScan = true;
  }

  loadFilterMode();
  Ao3TagMergeStore::load();
  loadSortFilterState();
  MARKED_FOR_LATER_STORE.loadFromFile();
  requestUpdate();
}

void Ao3LibraryActivity::loadFilterMode() {
    filterMode = FilterMode::AUTOMATIC;
    ao3Folder  = "";
    allowedHashes.clear();
    swapNavButtons = false;
    autoIndexOnOpen_ = false;

    const char* path = "/.crosspoint/ao3_settings.json";
    if (!Storage.exists(path)) return;

    String json = Storage.readFile(path);
    if (json.isEmpty()) return;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return;

    ao3Folder  = doc["ao3Folder"] | "";
    uint8_t fm = doc["filterMode"] | 0;
    filterMode = (fm == 1) ? FilterMode::FOLDER_TREE : FilterMode::AUTOMATIC;
    swapNavButtons = doc["swapNavButtons"] | false;
    autoIndexOnOpen_ = doc["autoIndexOnOpen"] | false;
    if (filterMode != FilterMode::FOLDER_TREE) {
        folderTreeFandom.clear();
        folderTreeRelationship.clear();
    }
}

void Ao3LibraryActivity::buildAllowedHashes(const std::string& scanPath, int maxDepth) {
    allowedHashes.clear();

    std::function<void(const std::string&, int)> scanRecursive = [&](const std::string& dirPath, int currentDepth) {
        //if (currentDepth > maxDepth) return;
        HalFile dir = Storage.open(dirPath.c_str());
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }

        char name[256];
        HalFile file;
        while (file = dir.openNextFile()) {
            file.getName(name, sizeof(name));
            std::string nameStr(name);
            if (file.isDirectory() && name[0] != '.' && nameStr != "System Volume Information") {
                std::string subPath = dirPath;
                if (subPath.back() != '/') subPath += "/";
                subPath += nameStr;
                scanRecursive(subPath, currentDepth + 1);
            } else if (!file.isDirectory() && currentDepth >= maxDepth) {
                size_t dotPos = nameStr.find_last_of('.');
                if (dotPos != std::string::npos) {
                    std::string ext = nameStr.substr(dotPos + 1);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == "epub") {
                        std::string fullPath = dirPath;
                        if (fullPath.back() != '/') fullPath += "/";
                        fullPath += nameStr;
                        uint32_t h = static_cast<uint32_t>(std::hash<std::string>{}(fullPath));
                        allowedHashes.push_back(h);
                    }
                }
            }
            file.close();
            yield();
        }
        dir.close();
    };

    scanRecursive(scanPath, 0);
    std::sort(allowedHashes.begin(), allowedHashes.end());
}

void Ao3LibraryActivity::onExit() {
    Activity::onExit();
    Ao3TagMergeStore::unload();
    MARKED_FOR_LATER_STORE.clearEntries();
    g_silentReturnHint = {};
}

// ---------------------------------------------------------------------------
//  loadViewEntries — delegates to rebuildViewEntries
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::loadViewEntries() {
  if (indexState != IndexState::UNKNOWN) return;
  rebuildViewEntries();
}

// ---------------------------------------------------------------------------
//  getBookStatus — reads the 7 or 10 byte progress.bin for the given cache hash
// ---------------------------------------------------------------------------

BookStatus Ao3LibraryActivity::getBookStatus(uint32_t cacheHash) {
  std::string cachePath = "/.crosspoint/epub_" + std::to_string(cacheHash) + "/progress.bin";
  HalFile f;
  if (Storage.openFileForRead("AO3L", cachePath, f)) {
    uint8_t data[11];
    int dataSize = f.read(data, sizeof(data));
    f.close();
    if (dataSize == 7)  return static_cast<BookStatus>(data[6]);
    if (dataSize == 11) return static_cast<BookStatus>(data[10]);
  }
  return BookStatus::START;
}

// ---------------------------------------------------------------------------
//  loadPageCache — pulls full Ao3LibraryMetadata for the visible 3 entries
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::loadPageCache(int page) {
  const int startIdx = page * 3;
  const int endIdx   = std::min(startIdx + 3, static_cast<int>(viewEntries.size()));

  // Zero all slots first
  for (int i = 0; i < 3; i++) {
    new (&pageCache[i]) Ao3LibraryMetadata();
    pageCacheStatus[i] = BookStatus::START;
    pageCacheMarkedPosition[i] = -1;
    pageCachePlanned[i] = 0;
  }

  for (int i = startIdx; i < endIdx; i++) {
    const int slot = i - startIdx;
    std::string infoPath =
        "/.crosspoint/epub_" + std::to_string(viewEntries[i].cacheHash) + "/ao3_library_info";
    HalFile f;
    if (Storage.openFileForRead("AO3L", infoPath, f)) {
      f.read((uint8_t*)&pageCache[slot], sizeof(Ao3LibraryMetadata));
      uint16_t planned = 0;
      if (f.read((uint8_t*)&planned, sizeof(planned)) == sizeof(planned)) pageCachePlanned[slot] = planned;
      f.close();
    }
    pageCacheStatus[slot] = getBookStatus(viewEntries[i].cacheHash);
    pageCacheMarkedPosition[slot] = MARKED_FOR_LATER_STORE.getQueuePosition(pageCache[slot].filepath);
  }

  cachedPage = page;

  // Pre-compute wrapped summary lines — zero CPU work in render()
  const int textWidth = renderer.getScreenWidth() - 40;
  for (int i = 0; i < 3; i++) {
    wrappedSummary[i].clear();
    if (pageCache[i].summary[0] != 0) {
      for (int j = 0; pageCache[i].summary[j] != '\0'; j++) {
        if (pageCache[i].summary[j] == '\n' || pageCache[i].summary[j] == '\r') {
          pageCache[i].summary[j] = ' ';
        }
      }
      wrappedSummary[i] = renderer.wrappedText(SMALL_FONT_ID, pageCache[i].summary, textWidth, 3);
    }
  }
}

// ---------------------------------------------------------------------------
//  loop
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::loop() {
  // Still loading — loadViewEntries() will flip indexState on first call
  if (indexState == IndexState::UNKNOWN) {
    if (autoIndexOnOpen_ && !autoIndexLaunched_ && !ao3Folder.empty()) {
      bool full = false;
      {
        HalFile f;
        if (Storage.openFileForRead("AO3L", "/.crosspoint/ao3_library_index.bin", f)) {
          char magic[4]; uint8_t version; uint16_t recordCount;
          if (f.read(magic, 4) == 4 && f.read(&version, 1) == 1 &&
              f.read((uint8_t*)&recordCount, 2) == 2 &&
              memcmp(magic, "AO3X", 4) == 0) {
            f.seek(12); // skip rest of header
            uint16_t liveCount = 0;
            CompactIndexRecord rec;
            for (uint16_t i = 0; i < recordCount; i++) {
              if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
              if (!(rec.flags & 0x01)) liveCount++;
            }
            full = (liveCount >= MAX_LIBRARY_BOOKS);
          }
          f.close();
        }
      }
      if (pendingTransferScan && !full) {
        pendingTransferScan = false; // Reset the flag so it won't scan again until a new book is added
        autoIndexLaunched_ = true;
        viewEntries.clear();
        viewEntries.shrink_to_fit();
        auto handler = [this](const ActivityResult&) {
          indexState = IndexState::UNKNOWN;
          rebuildViewEntries();
          requestUpdate();
        };

        // Pass autoFinishIfEmpty = true, headless = true
        startActivityForResult(
            std::make_unique<Ao3IndexActivity>(renderer, mappedInput, Ao3IndexMode::DIRECTORY, "", true, true),
            handler);
        return;
      }
    }
    loadViewEntries();
    requestUpdate();
    return;
  }

  // --- STATE: LIBRARY ---
  if (screenState == ScreenState::LIBRARY) {

    if (!swapNavButtons) {
      // Default: side buttons open panels, front left/right scroll
      if (mappedInput.wasReleased(MappedInputManager::Button::Up ) &&
          mappedInput.getHeldTime() < 500) {
        screenState = ScreenState::MANAGE_PANEL;
        managePanelRowIndex = 0;
        requestUpdate(true);
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Down)&&
          mappedInput.getHeldTime() < 500) {
        screenState = ScreenState::FILTER_PANEL;
        pendingState = activeState;
        overlayRowIndex = 0;
        overlayScroll_ = 0;
        requestUpdate(true);
        return;
      }
      const int total = static_cast<int>(viewEntries.size());
      if (total > 0) {
        buttonNavigator.onPress({MappedInputManager::Button::Right}, [this, total] {
          selectorIndex = (selectorIndex + 1) % total;
          requestUpdate();
        });
        buttonNavigator.onPress({MappedInputManager::Button::Left}, [this, total] {
          selectorIndex = (selectorIndex + total - 1) % total;
          requestUpdate();
        });
        buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this, total] {
          selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, total, 3);
          requestUpdate();
        });
        buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this, total] {
          selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, total, 3);
          requestUpdate();
        });
        // Page skip on hold for panel buttons 
        buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [this, total] {
          selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, total, 3);
          requestUpdate();
        });
        buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [this, total] {
          selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, total, 3);
          requestUpdate();
        });
      }
    } else {
      // Swapped: front left/right open panels, side buttons scroll
      if (mappedInput.wasReleased(MappedInputManager::Button::Right) &&
          mappedInput.getHeldTime() < 500) {
        screenState = ScreenState::FILTER_PANEL;
        pendingState = activeState;
        overlayRowIndex = 0;
        overlayScroll_ = 0;
        requestUpdate(true);
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Left) &&
          mappedInput.getHeldTime() < 500) {
        screenState = ScreenState::MANAGE_PANEL;
        managePanelRowIndex = 0;
        requestUpdate(true);
        return;
      }
      const int total = static_cast<int>(viewEntries.size());
      if (total > 0) {
        buttonNavigator.onPress({MappedInputManager::Button::Down}, [this, total] { 
          selectorIndex = (selectorIndex + 1) % total;
          requestUpdate();
        });
        buttonNavigator.onPress({MappedInputManager::Button::Up}, [this, total] { 
          selectorIndex = (selectorIndex + total - 1) % total;
          requestUpdate();
        });
        buttonNavigator.onSideNextContinuous([this, total] {
          selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, total, 3);
          requestUpdate();
        });
        buttonNavigator.onSidePreviousContinuous([this, total] {
          selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, total, 3);
          requestUpdate();
        });
        // Page skip on hold for panel buttons
        buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this, total] {
          selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, total, 3);
          requestUpdate();
        });
        buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this, total] {
          selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, total, 3);
          requestUpdate();
        });
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (skipNextBackRelease) {
        skipNextBackRelease = false;
      } else {
        activityManager.popActivity();
      }
      return;
    }

    // --- Touch (additive; the button handling around it is untouched) ---
    // touchOpen = tap on an entry's title+author band, touchHold = long-press on
    // one. Both feed the existing Confirm / Confirm-hold path below, so opening
    // the reader and the BookAction menu stay implemented in a single place.
    bool touchOpen = false;
    bool touchHold = false;
    if (mappedInput.hasTouch()) {
      const int sw = renderer.getScreenWidth();
      const int hintsH = UITheme::getInstance().getMetrics().buttonHintsHeight;
      const int arrowY = bottomArrowBaseY(renderer.getScreenHeight(), hintsH) + hintsH / 2;
      int tx = 0;
      int ty = 0;

      // Page flip: swipe up = next page, swipe down = previous, wrapping at the ends.
      const auto swipe = mappedInput.wasSwipe();
      if ((swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) &&
          viewEntries.size() > 3) {
        const int pages = (static_cast<int>(viewEntries.size()) + 2) / 3;
        const int cur = static_cast<int>(selectorIndex) / 3;
        const int next = (swipe == MappedInputManager::SwipeDir::Up) ? (cur + 1) % pages : (cur + pages - 1) % pages;
        selectorIndex = static_cast<size_t>(next) * 3;
        requestUpdate();
        return;
      }

      // Title+author band of each visible entry (the dithered selection area), or -1.
      // Horizontal span mirrors renderEntry: textX - 8 .. screenWidth - 15, where
      // textX = 20 + 56 + 15; band height = selectionHeight (56) + 6.
      auto entryAt = [&](const int x, const int y) -> int {
        const int bandTop = contentStartPx_ - 3;
        if (entrySlotPx_ <= 0 || x < 20 + 56 + 15 - 8 || x >= sw - 15 || y < bandTop) return -1;
        const int row = (y - bandTop) / entrySlotPx_;
        if (row > 2 || (y - bandTop) % entrySlotPx_ >= 56 + 6) return -1;
        const int idx = (static_cast<int>(selectorIndex) / 3) * 3 + row;
        return idx < static_cast<int>(viewEntries.size()) ? idx : -1;
      };

      if (mappedInput.wasScreenTapped(tx, ty)) {
        // Tapping anywhere in the 48px Top Header band opens the filter panel
        if (ty < 48) {
          screenState = ScreenState::FILTER_PANEL;
          pendingState = activeState;
          overlayRowIndex = 0;
          requestUpdate(true);
          return;
        }
        
        // Tapping anywhere in the bottom 56px band opens the manage panel
        if (ty >= renderer.getScreenHeight() - 56) {
          screenState = ScreenState::MANAGE_PANEL;
          managePanelRowIndex = 0;
          requestUpdate(true);
          return;
        }
        
        const int hit = entryAt(tx, ty);
        if (hit >= 0) {
          selectorIndex = static_cast<size_t>(hit);
          touchOpen = true;
        }
      } else if (mappedInput.isScreenTouchHeld(tx, ty) && entryAt(tx, ty) >= 0 &&
                 mappedInput.wasScreenLongPress(tx, ty)) {
        // Checked against the live position first: wasScreenLongPress() suppresses
        // the whole contact as soon as it is consumed, wherever the finger is.
        selectorIndex = static_cast<size_t>(entryAt(tx, ty));
        touchHold = true;
      }
    }

    if (!viewEntries.empty() && (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || touchOpen || touchHold)) {
      // Ensure the page cache is fresh for the current selector position
      const int selPage = static_cast<int>(selectorIndex) / 3;
      if (selPage != cachedPage) loadPageCache(selPage);

      const int slot = static_cast<int>(selectorIndex) % 3;
      // Full epub path and title live in the ao3_library_info sidecar (page cache)
      const std::string epubPath(pageCache[slot].filepath);
      const std::string epubTitle(
          pageCache[slot].title[0] ? pageCache[slot].title : viewEntries[selectorIndex].title);
      const uint32_t hash = viewEntries[selectorIndex].cacheHash;

      if ((touchHold || (!touchOpen && mappedInput.getHeldTime() >= 1000)) && !epubPath.empty()) {
        // Long-press → BookAction menu
        auto handler = [this, epubPath, hash](const ActivityResult& res) {
          g_silentReturnHint = {};
          if (const auto* actionRes = std::get_if<BookActionResult>(&res.data)) {
            if (actionRes->modified) {
             if (actionRes->deleted || actionRes->archived) {
                if (actionRes->deleted) {
                  Ao3Librarian::tombstoneRecord(epubPath);
                  if (Storage.remove(epubPath.c_str())) {
                    Epub(epubPath, "/.crosspoint").clearCache();
                  }
                }
                // archived: tombstone + move + cache re-key already done inside archiveFic()
                // just remove from in-RAM view

                auto it = std::find_if(viewEntries.begin(), viewEntries.end(),
                   [hash](const ViewEntry& v) { return v.cacheHash == hash; });
                if (it != viewEntries.end()) viewEntries.erase(it);

                if (!viewEntries.empty()) {
                  if (selectorIndex >= viewEntries.size())
                    selectorIndex = viewEntries.size() - 1;
                } else {
                  selectorIndex = 0;
                }
               cachedPage = -1;
              } else if (actionRes->indexingCompleted) {
                rebuildViewEntries();
              } else {
                loadPageCache(cachedPage);
                auto it = std::find_if(viewEntries.begin(), viewEntries.end(),
                                       [hash](const ViewEntry& v) { return v.cacheHash == hash; });
                if (it != viewEntries.end()) {
                  uint8_t bits = static_cast<uint8_t>(it->filterBits & ~(kFilterStatusMask | kFilterFavourite));
                  bits = static_cast<uint8_t>(bits | (static_cast<uint8_t>(actionRes->newStatus) & kFilterStatusMask));
                  const int flags = Ao3Librarian::getRecordFlags(epubPath);
                  if (flags >= 0) {
                    if (flags & kIndexFavourite) bits = static_cast<uint8_t>(bits | kFilterFavourite);
                    if (flags & kIndexMetaCompleted) bits = static_cast<uint8_t>(bits | kFilterMetaCompleted);
                    else bits = static_cast<uint8_t>(bits & ~kFilterMetaCompleted);
                  }
                  it->filterBits = bits;
                  if (!passesFilter(*it, computeFilterHashes(activeState))) {
                    viewEntries.erase(it);
                    if (!viewEntries.empty() && selectorIndex >= viewEntries.size())
                      selectorIndex = viewEntries.size() - 1;
                    cachedPage = -1;
                  }
                }
             }
              requestUpdate(true);
            }
          }
        };
        g_silentReturnHint = {SILENT_TARGET_AO3_LIBRARY, 0, static_cast<int16_t>(selectorIndex)};
        startActivityForResult(
            std::make_unique<BookActionActivity>(renderer, mappedInput, epubPath, epubTitle),
            handler);
      } else if (!epubPath.empty()) {
        APP_STATE.ao3LibraryReturnIndex = static_cast<int>(selectorIndex);
        APP_STATE.dashboardReturnTab = -1;
        APP_STATE.dashboardReturnIndex = -1;
        activityManager.goToReader(epubPath);
      }
      return;
    }
  }

  // --- STATE: FILTER_PANEL ---
  else if (screenState == ScreenState::FILTER_PANEL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      screenState = ScreenState::LIBRARY;
      requestUpdate(true);
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (mappedInput.getHeldTime() >= 500) {
        overlayRowIndex = FR_APPLY;
      } else {
        overlayRowIndex = (overlayRowIndex + 1) % FR_COUNT;
        if (overlayRowIndex == FR_REL && pendingState.fandom[0] == '\0') overlayRowIndex = FR_CLEAR_REL;
      }
      requestUpdate(true);
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (mappedInput.getHeldTime() >= 500) {
        overlayRowIndex = FR_APPLY;
      } else {
        overlayRowIndex = (overlayRowIndex + FR_COUNT - 1) % FR_COUNT;
        if (overlayRowIndex == FR_REL && pendingState.fandom[0] == '\0') overlayRowIndex = FR_CLEAR_FANDOM;
      }
      requestUpdate(true);
      return;
    }

    bool executeFilterAction = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    int tx = 0, ty = 0;

    if (mappedInput.wasScreenTapped(tx, ty)) {
      if (ty < overlayTop_ || ty >= overlayBottom_) {
        screenState = ScreenState::LIBRARY;
        requestUpdate(true);
        return;
      }
      const int row = overlayScroll_ + (ty - overlayRow0Y_) / overlayRowPitch_;
      if (row >= 0 && row < FR_COUNT) {
        overlayRowIndex = row;
        if (row == FR_APPLY) render(RenderLock(*this));
        executeFilterAction = true;
      }
    }

    if (executeFilterAction) {
      if (overlayRowIndex == FR_CLEAR_ALL) {
        clearAllFilters();
      } else if (overlayRowIndex == FR_FANDOM) {
        // Fandom row: cycle if <= 4 fandoms, open full picker if > 4
        pickerItems.clear();
        pickerItems.push_back("Any");
        {
          std::vector<std::string> fandoms;
          buildFandomList(fandoms);
          pickerItems.reserve(fandoms.size() + 1);
          for (auto& f : fandoms) pickerItems.push_back(std::move(f));
        }
        if (pickerItems.size() <= 5) {
          size_t currentIdx = 0;
          for (size_t i = 0; i < pickerItems.size(); i++) {
            if (filterMode == FilterMode::FOLDER_TREE
                    ? folderTreeFandom == pickerItems[i]
                    : strcmp(pendingState.fandom, pickerItems[i].c_str()) == 0) {
              currentIdx = i;
              break;
            }
          }
          currentIdx = (currentIdx + 1) % pickerItems.size();
          if (currentIdx == 0) {
            pendingState.fandom[0] = '\0';
            pendingState.relationship[0] = '\0';
            pendingState.relationshipNoneOnly = false;
            if (filterMode == FilterMode::FOLDER_TREE) {
                folderTreeFandom.clear();
                folderTreeRelationship.clear();
            }
          } else {
            strncpy(pendingState.fandom, pickerItems[currentIdx].c_str(), 31);
            pendingState.fandom[31] = '\0';
            pendingState.relationship[0] = '\0';
            pendingState.relationshipNoneOnly = false;
            if (filterMode == FilterMode::FOLDER_TREE) {
                folderTreeFandom = pickerItems[currentIdx];
                folderTreeRelationship.clear();
            }
          }
        } else {
          int targetIdx = 0;
          for (size_t i = 0; i < pickerItems.size(); i++) {
            if (filterMode == FilterMode::FOLDER_TREE
                    ? folderTreeFandom == pickerItems[i]
                    : strcmp(pendingState.fandom, pickerItems[i].c_str()) == 0) {
              targetIdx = i;
              break;
            }
          }
          launchPicker(ScreenState::FANDOM_PICKER, targetIdx);
        }
      } else if (overlayRowIndex == FR_CLEAR_FANDOM) {
        clearFandomFilter();
      } else if (overlayRowIndex == FR_REL) {
        // Relationship row: always opens a list picker if fandom is active
        if (pendingState.fandom[0] != '\0') {
          screenState = ScreenState::RELATIONSHIP_PICKER;
          pickerItems.clear();
          pickerItems.push_back("Any");
          {
            std::vector<std::string> rels;
            buildRelationshipList(
            (filterMode == FilterMode::FOLDER_TREE ? folderTreeFandom.c_str() : pendingState.fandom),
              rels, pickerHasNone);
            if (pickerHasNone) pickerItems.push_back("None");
            pickerItems.reserve(pickerItems.size() + rels.size());
            for (auto& r : rels) pickerItems.push_back(std::move(r));
          }
          int targetIdx = 0;
          if (pendingState.relationshipNoneOnly) {
            if (pickerHasNone) targetIdx = 1;
          } else if (pendingState.relationship[0] != '\0') {
            for (size_t i = 0; i < pickerItems.size(); i++) {
              if (filterMode == FilterMode::FOLDER_TREE
                      ? folderTreeRelationship == pickerItems[i]
                      : strcmp(pendingState.relationship, pickerItems[i].c_str()) == 0) {
                targetIdx = i;
                break;
              }
            }
          }
          launchPicker(ScreenState::RELATIONSHIP_PICKER, targetIdx);
        }
      } else if (overlayRowIndex == FR_CLEAR_REL) {
        pendingState.relationship[0] = '\0';
        pendingState.relationshipNoneOnly = false;
        folderTreeRelationship.clear();
      } else if (overlayRowIndex == FR_TITLE) {
        letterForTitle = true;
        letterRow = 0;
        letterCol = 0;
        screenState = ScreenState::LETTER_GRID;
      } else if (overlayRowIndex == FR_CLEAR_TITLE) {
        pendingState.titleInitials = 0;
      } else if (overlayRowIndex == FR_AUTHOR) {
        letterForTitle = false;
        letterRow = 0;
        letterCol = 0;
        screenState = ScreenState::LETTER_GRID;
      } else if (overlayRowIndex == FR_CLEAR_AUTHOR) {
        pendingState.authorInitials = 0;
      } else if (overlayRowIndex == FR_STATUS) {
        statusRow = 0;
        screenState = ScreenState::STATUS_TICKS;
      } else if (overlayRowIndex == FR_CLEAR_STATUS) {
        pendingState.statusMask = 0;
        pendingState.publicationMask = 0;
        pendingState.favouriteOnly = false;
        pendingState.combine = CombineMode::ALL;
      } else if (overlayRowIndex == FR_TEXT) {
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Find text or tag", pendingState.textQuery,
                                                    31),
            [this](const ActivityResult& res) {
              if (!res.isCancelled) {
                if (const auto* typed = std::get_if<KeyboardResult>(&res.data)) {
                  strncpy(pendingState.textQuery, typed->text.c_str(), 31);
                  pendingState.textQuery[31] = '\0';
                }
              }
              screenState = ScreenState::FILTER_PANEL;
              requestUpdate(true);
            });
      } else if (overlayRowIndex == FR_CLEAR_TEXT) {
        pendingState.textQuery[0] = '\0';
      } else if (overlayRowIndex == FR_SORT) {
        switch (pendingState.sortMode) {
          case SortMode::ALPHABETIC: pendingState.sortMode = SortMode::AUTHOR; break;
          case SortMode::AUTHOR: pendingState.sortMode = SortMode::WORD_COUNT; break;
          case SortMode::WORD_COUNT: pendingState.sortMode = SortMode::DATE_ADDED; break;
          case SortMode::DATE_ADDED: pendingState.sortMode = SortMode::SERIES; break;
          case SortMode::SERIES: pendingState.sortMode = SortMode::ALPHABETIC; break;
        }
      } else if (overlayRowIndex == FR_ORDER) {
        pendingState.ascending = !pendingState.ascending;
      } else if (overlayRowIndex == FR_APPLY) {
        commitPendingFilters();
        return;
      }
      requestUpdate(true);
      return;
    }
  }

  // --- STATE: LETTER GRID ---
  else if (screenState == ScreenState::LETTER_GRID) {
    auto* mask = letterForTitle ? &pendingState.titleInitials : &pendingState.authorInitials;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      screenState = ScreenState::FILTER_PANEL;
      requestUpdate(true);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (letterRow >= 4) letterCol = 0;
      else letterCol = (letterCol + letterRowCount(letterRow) - 1) % letterRowCount(letterRow);
      requestUpdate(true);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (letterRow >= 4) letterCol = 1;
      else letterCol = (letterCol + 1) % letterRowCount(letterRow);
      requestUpdate(true);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      letterRow = (letterRow + 4) % 5;
      if (letterRow < 4 && letterCol >= letterRowCount(letterRow)) letterCol = letterRowCount(letterRow) - 1;
      if (letterRow >= 4 && letterCol > 1) letterCol = 0;
      requestUpdate(true);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      letterRow = (letterRow + 1) % 5;
      if (letterRow < 4 && letterCol >= letterRowCount(letterRow)) letterCol = letterRowCount(letterRow) - 1;
      if (letterRow >= 4 && letterCol > 1) letterCol = 0;
      requestUpdate(true);
      return;
    }
    bool toggle = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const int originY = metrics.topPadding + metrics.headerHeight + 28;
      KeyBox keys[40];
      const int n = layoutLetterKeys(renderer.getScreenWidth(), originY, keys, 40);
      for (int i = 0; i < n; i++) {
        if (tx >= keys[i].x && tx < keys[i].x + keys[i].w && ty >= keys[i].y && ty < keys[i].y + keys[i].h) {
          if (keys[i].apply) {
            letterRow = 4;
            letterCol = 1;
          } else if (keys[i].clear) {
            letterRow = 4;
            letterCol = 0;
          } else {
            letterRow = 0;
            int seen = 0;
            for (int r = 0; r < 4; r++) {
              if (i < seen + letterRowCount(r)) {
                letterRow = r;
                letterCol = i - seen;
                break;
              }
              seen += letterRowCount(r);
            }
          }
          toggle = true;
          break;
        }
      }
    }
    if (toggle) {
      if (letterRow >= 4) {
        if (letterCol == 1) {
          commitPendingFilters();
          return;
        }
        *mask = 0;
      } else {
        const char ch = letterAt(letterRow, letterCol);
        const int bit = initialBit(static_cast<unsigned char>(ch));
        if (bit >= 0) *mask ^= (uint64_t{1} << bit);
      }
      requestUpdate(true);
    }
  }

  // --- STATE: STATUS TICKS ---
  else if (screenState == ScreenState::STATUS_TICKS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      screenState = ScreenState::FILTER_PANEL;
      requestUpdate(true);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      statusRow = (statusRow + kStatusRows - 1) % kStatusRows;
      requestUpdate(true);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      statusRow = (statusRow + 1) % kStatusRows;
      requestUpdate(true);
      return;
    }
    bool toggle = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const int row = (ty - statusTop_) / statusPitch_;
      if (row >= 0 && row < kStatusRows) {
        statusRow = row;
        toggle = true;
      }
    }
    if (toggle) {
      if (statusRow == 0) {
        pendingState.combine = pendingState.combine == CombineMode::ALL ? CombineMode::ANY : CombineMode::ALL;
      } else if (statusRow == 1) {
        pendingState.favouriteOnly = !pendingState.favouriteOnly;
      } else if (statusRow >= 2 && statusRow <= 6) {
        pendingState.statusMask = static_cast<uint8_t>(pendingState.statusMask ^ (1u << (statusRow - 2)));
      } else if (statusRow == 7) {
        pendingState.publicationMask = static_cast<uint8_t>(pendingState.publicationMask ^ 0x01);
      } else if (statusRow == 8) {
        pendingState.publicationMask = static_cast<uint8_t>(pendingState.publicationMask ^ 0x02);
      } else if (statusRow == 9) {
        commitPendingFilters();
        return;
      }
      requestUpdate(true);
    }
  }

  // --- STATE: PICKERS ---
  else if (screenState == ScreenState::FANDOM_PICKER || screenState == ScreenState::RELATIONSHIP_PICKER) {
    // 1. Route touch tap via FreeInkApp
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) {
      requestUpdate(true);
    }
    if (route) {
      return; // Handled and dispatched to onPickerRowActionTrampoline
    }

    // 2. Touch swipe gestures scroll the list without changing selection
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      bool moved = false;
      {
        RenderLock lock(*this);
        const int delta = (swipe == MappedInputManager::SwipeDir::Up) ? pickerNav.pageRows() : -pickerNav.pageRows();
        moved = pickerNav.scrollBy(delta, pickerItems.size());
      }
      if (moved) requestUpdate(true);
      return;
    }

    // 3. Back button (also triggered by touch left-to-right edge swipe gesture)
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeRouting();
      pickerRowItems.clear();
      screenState = ScreenState::FILTER_PANEL;
      requestUpdate(true);
      return;
    }

    // 4. Physical button navigation using ButtonNavigator and ListNav
    const int total = static_cast<int>(pickerItems.size());
    if (total > 0) {
      buttonNavigator.onNextPress([this, total] {
        pickerNav.selected = ButtonNavigator::nextIndex(pickerNav.selected, total);
        pickerNav.follow(total);
        requestUpdate(true);
      });
      buttonNavigator.onPreviousPress([this, total] {
        pickerNav.selected = ButtonNavigator::previousIndex(pickerNav.selected, total);
        pickerNav.follow(total);
        requestUpdate(true);
      });
      buttonNavigator.onFrontNextContinuous([this, total] {
        pickerNav.selected = (pickerNav.selected + 2) % total;
        pickerNav.follow(total);
        requestUpdate(true);
      });
      buttonNavigator.onFrontPreviousContinuous([this, total] {
        pickerNav.selected = (pickerNav.selected + total - 2) % total;
        pickerNav.follow(total);
        requestUpdate(true);
      });
      buttonNavigator.onSideNextContinuous([this, total] {
        pickerNav.selected = ButtonNavigator::nextPageIndex(pickerNav.selected, total, pickerNav.pageRows());
        pickerNav.follow(total);
        requestUpdate(true);
      });
      buttonNavigator.onSidePreviousContinuous([this, total] {
        pickerNav.selected = ButtonNavigator::previousPageIndex(pickerNav.selected, total, pickerNav.pageRows());
        pickerNav.follow(total);
        requestUpdate(true);
      });
    }

    // 5. Hardware Confirm button
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      confirmPickerSelection(pickerNav.selected);
      return;
    }
  }

  // --- STATE: MANAGE_PANEL ---
  else if (screenState == ScreenState::MANAGE_PANEL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      screenState = ScreenState::LIBRARY;
      requestUpdate(true);
      return;
    }

    // Up/Down/Left/Right all cycle between the 2 entries
    auto cycleNext = [this] {
      managePanelRowIndex = (managePanelRowIndex + 1) % 2;
      requestUpdate(true);
    };
    auto cyclePrev = [this] {
      managePanelRowIndex = (managePanelRowIndex + 1) % 2;
      requestUpdate(true);
    };

    if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      cycleNext();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      cyclePrev();
      return;
    }

    bool executeManagePanel = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const int panelH = 216;
      const int panelY = renderer.getScreenHeight() - panelH - metrics.buttonHintsHeight;
      
      if (ty < panelY) {
        // Tap outside: close panel
        screenState = ScreenState::LIBRARY;
        requestUpdate(true);
        return;
      } else {
        const int firstRowY = panelY + 62;
        if (ty >= firstRowY && ty <= firstRowY + 52) {
          managePanelRowIndex = 0;
          executeManagePanel = true;
        } else if (ty >= firstRowY + 60 && ty <= firstRowY + 60 + 52) {
          managePanelRowIndex = 1;
          executeManagePanel = true;
        }
      }
    }

    if (executeManagePanel) {
      if (managePanelRowIndex == 0) {
        // "Index New Books" — check if ao3 folder is configured first
        const char* settingsPath = "/.crosspoint/ao3_settings.json";
        bool hasFolder = false;
        if (Storage.exists(settingsPath)) {
          String json = Storage.readFile(settingsPath);
          if (!json.isEmpty()) {
            JsonDocument doc;
            if (!deserializeJson(doc, json)) {
              std::string folder = doc["ao3Folder"] | "";
              hasFolder = !folder.empty();
            }
          }
        }
        if (!hasFolder) {
          // Redirect to settings
          screenState = ScreenState::MANAGE_PANEL; // close panel first
          FilterMode oldMode = filterMode;
          std::string oldFolder = ao3Folder;
          auto handler = [this, oldMode, oldFolder](const ActivityResult&) {
            loadFilterMode();
            bool modeChanged = (oldMode != filterMode);
            bool folderChangedInTreeMode = (filterMode == FilterMode::FOLDER_TREE && oldFolder != ao3Folder);
            if (modeChanged || folderChangedInTreeMode) {
                memset(activeState.fandom, 0, 32);
                memset(activeState.relationship, 0, 32);
                activeState.relationshipNoneOnly = false;
                saveSortFilterState();
            }
            rebuildViewEntries();
            requestUpdate(true);
          };
          startActivityForResult(std::make_unique<Ao3LibrarySettingsActivity>(renderer, mappedInput), handler);
        } else {
          // Unload ViewEntry vector to free RAM, then launch Ao3IndexActivity
          viewEntries.clear();
          viewEntries.shrink_to_fit();
          indexState = IndexState::UNKNOWN; // mark for rebuild on return
          screenState = ScreenState::LIBRARY;
          auto handler = [this](const ActivityResult&) {
            rebuildViewEntries();
            requestUpdate(true);
          };
          startActivityForResult(std::make_unique<Ao3IndexActivity>(renderer, mappedInput, Ao3IndexMode::DIRECTORY), handler);
        }
      } else {
        // "AO3 Library Settings"
        screenState = ScreenState::LIBRARY;
        FilterMode oldMode = filterMode;
        std::string oldFolder = ao3Folder;
        auto handler = [this, oldMode, oldFolder](const ActivityResult&) {
          loadFilterMode();
          bool modeChanged = (oldMode != filterMode);
          bool folderChangedInTreeMode = (filterMode == FilterMode::FOLDER_TREE && oldFolder != ao3Folder);
          if (modeChanged || folderChangedInTreeMode) {
              memset(activeState.fandom, 0, 32);
              memset(activeState.relationship, 0, 32);
              activeState.relationshipNoneOnly = false;
              saveSortFilterState();
          }
          rebuildViewEntries();
          requestUpdate(true);
        };
        startActivityForResult(std::make_unique<Ao3LibrarySettingsActivity>(renderer, mappedInput), handler);
      }
      return;
    }
  }
}

// ---------------------------------------------------------------------------
//  render
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::render(RenderLock&& lock) {
  if (screenState == ScreenState::FANDOM_PICKER || screenState == ScreenState::RELATIONSHIP_PICKER) {
    renderPicker();
    return;
  }
  if (screenState == ScreenState::LETTER_GRID) {
    renderLetterGrid();
    return;
  }
  if (screenState == ScreenState::STATUS_TICKS) {
    renderStatusTicks();
    return;
  }

  // MANAGE_PANEL is a full-screen composited: library background + bottom slide-up panel
  renderLibrary(lock);

  if (screenState == ScreenState::FILTER_PANEL) {
    renderFilterOverlay();
  } else if (screenState == ScreenState::MANAGE_PANEL) {
    renderManagePanel();
  }

  renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
//  renderLibrary — renders standard book list (or empty messages)
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::renderLibrary(RenderLock& lock) {
  renderer.clearScreen();

  // Draw Header Title (Truncate to 25 chars if fandom filter is active)
  char headerTitle[32] = "AO3 Library";
  if (activeState.fandom[0] != '\0') {
    std::string cleanFandom(activeState.fandom);
    if (cleanFandom.length() > 29) {
      cleanFandom = cleanFandom.substr(0, 27) + "..";
    }
    strcpy(headerTitle, cleanFandom.c_str());
  }
  renderer.drawText(UI_12_FONT_ID, 15, 12, headerTitle, true, EpdFontFamily::BOLD);
  renderer.drawLine(0, 48, renderer.getScreenWidth(), 48);

  // States handling
  if (indexState == IndexState::UNKNOWN) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, "Loading...");
    return;
  }
    // Triangle indicator in header — pointing down when filter closed, up when open, also up for MANAGE_PANEL
    {
      const int tx = renderer.getScreenWidth() - 26;
      const int ty = 26;
      if (screenState == ScreenState::FILTER_PANEL) {
        // triangle with black border pointing up (filter is open)
        const int xPts[] = { tx, tx + 12, tx + 6 };
        const int yPts[] = { ty + 5, ty + 5, ty - 5 };
        renderer.fillPolygon(xPts, yPts, 3, Black);
      } else {
        // Solid black triangle pointing down (filter accessible via Down button)
        const int xPts[] = { tx, tx + 12, tx + 6 };
        const int yPts[] = { ty - 5, ty - 5, ty + 5 };
        renderer.fillPolygon(xPts, yPts, 3, Black);
      }
    }

  if (indexState == IndexState::MISSING || (indexState == IndexState::OK && viewEntries.empty())) {
    if (activeState.fandom[0] != '\0') {
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 12, "No matches for current filter.");
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 12, "No AO3 books indexed yet.");
      renderer.drawCenteredText(SMALL_FONT_ID, renderer.getScreenHeight() / 2 + 12, "Press SIDE UP to index new books.");
    }
  } else if (indexState == IndexState::CORRUPT) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 12, "Library index missing or corrupt.");
    renderer.drawCenteredText(SMALL_FONT_ID, renderer.getScreenHeight() / 2 + 12, "Open a book to recreate it.");
  } else {
    const int startIdx = (selectorIndex / 3) * 3;
    const int endIdx   = std::min(startIdx + 3, static_cast<int>(viewEntries.size()));

    const auto& metrics = UITheme::getInstance().getMetrics();
    const int   topPad  = 18;

    const int contentEnd   = renderer.getScreenHeight() - metrics.buttonHintsHeight;
    const int entrySlot    = (contentEnd - (42 + topPad)) / 3;
    const int contentStart = 48 + topPad;
    entrySlotPx_    = entrySlot;     // recorded for touch hit-testing in loop()
    contentStartPx_ = contentStart;

    const int currentPage = static_cast<int>(selectorIndex) / 3;
    if (currentPage != cachedPage) {
      loadPageCache(currentPage);
    }

    int y = contentStart;
    for (int i = startIdx; i < endIdx; i++) {
      const bool selected = (i == static_cast<int>(selectorIndex)) && (screenState == ScreenState::LIBRARY);
      renderEntry(lock, y, viewEntries[i], i - startIdx, selected);
      y += entrySlot;
      if (i < endIdx - 1) {
        renderer.drawLine(15, y - topPad, renderer.getScreenWidth() - 15, y - topPad);
      }
    }

    // Page counter
    if (static_cast<int>(viewEntries.size()) > 3) {
      char pageBuf[32];
      sprintf(pageBuf, "%d / %d",
              (startIdx / 3) + 1,
              (static_cast<int>(viewEntries.size()) + 2) / 3);
      const int counterX =
          renderer.getScreenWidth() - 36 - renderer.getTextWidth(SMALL_FONT_ID, pageBuf);
      renderer.drawText(SMALL_FONT_ID, counterX, 15, pageBuf);
    }
  }

  // Draw Button hints

    const auto labels = (swapNavButtons && screenState == ScreenState::LIBRARY)
      ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "Manage", "Filter")
      : mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    // Triangle indicator: Show 'Up' triangle when Manage Panel is closed
    if (screenState != ScreenState::MANAGE_PANEL) {

      const auto& metrics = UITheme::getInstance().getMetrics();
      const int tx = renderer.getScreenWidth() - 26;
      const int hintsY = bottomArrowBaseY(renderer.getScreenHeight(), metrics.buttonHintsHeight);
      const int ty = hintsY + metrics.buttonHintsHeight / 2;

      // ▲ pointing up
      const int xPts[] = { tx + 6, tx, tx + 12 };
      const int yPts[] = { ty - 5, ty + 5, ty + 5 };
      renderer.fillPolygon(xPts, yPts, 3, Black);
    }
}

// ---------------------------------------------------------------------------
//  renderManagePanel — "Manage AO3 Library" slide-up panel from bottom
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::renderManagePanel() {
  const int screenWidth  = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const auto& metrics    = UITheme::getInstance().getMetrics();

  // Panel dimensions: 216px tall, anchored to bottom above the hints bar
  const int panelH     = 216;
  const int panelY     = screenHeight - panelH - metrics.buttonHintsHeight;
  const int margin     = 20;
  const int rowHeight  = 52;
  const int rowGap    = 8;

  // White filled background
  renderer.fillRoundedRect(0, panelY, screenWidth, panelH + metrics.buttonHintsHeight, 0, White);
  // Top 6px shadow line
  for (int i = 0; i < 6; i++) {
    renderer.drawLine(0, panelY + i, screenWidth, panelY + i);
  }

  // Panel header
  renderer.drawText(UI_12_FONT_ID, margin + 12, panelY + 22, "Manage AO3 Library", true, EpdFontFamily::BOLD);

  // Row definitions
  const char* rowLabels[2] = { "Index New Books", "Settings" };
  const char* rowDescs[2]  = {
    "Scan AO3 folder & find unindexed epubs",
    "Set AO3 folder & Filter mode"
  };

  const int firstRowY = panelY + 62;
  for (int i = 0; i < 2; i++) {
    const int rowY     = firstRowY + i * (rowHeight + rowGap);
    const bool sel     = (managePanelRowIndex == i);

    if (sel) {
      renderer.fillRoundedRect(margin, rowY,
                               screenWidth - 2 * margin, rowHeight, 6,
                               true, true, true, true, LightGray);
    }

    renderer.drawText(UI_10_FONT_ID, margin + 12, rowY + 4, rowLabels[i], true);
    renderer.drawText(SMALL_FONT_ID, margin + 12, rowY + 26, rowDescs[i]);
  }

  // Button hints override for the panel
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Select", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Manage panel triangle indicator — in button hints bar, same X as header triangle
  {
    const int tx = renderer.getScreenWidth() - 26;
    const int hintsY = bottomArrowBaseY(renderer.getScreenHeight(), metrics.buttonHintsHeight);
    const int ty = hintsY + metrics.buttonHintsHeight / 2;
    if (screenState == ScreenState::MANAGE_PANEL) {

        // ▼ pointing down when panel is open
        const int xPts[] = { tx, tx + 12, tx + 6 };
        const int yPts[] = { ty - 5, ty - 5, ty + 5 };
        renderer.fillPolygon(xPts, yPts, 3, Black);
    }
  }
}

void Ao3LibraryActivity::renderFilterOverlay() {
  const int screenWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int startY = 40;
  const int hints = metrics.buttonHintsHeight > 0 ? metrics.buttonHintsHeight : kBottomArrowRaise;
  const int overlayH = renderer.getScreenHeight() - hints - startY;
  overlayTop_ = startY;
  overlayBottom_ = startY + overlayH;
  const int margin = 12;
  const int headerH = 26;
  int pitch = (overlayH - headerH - 4) / FR_COUNT;
  if (pitch > 28) pitch = 28;
  if (pitch < 22) pitch = 22;
  int visible = (overlayH - headerH - 4) / pitch;
  if (visible < 1) visible = 1;
  if (visible > FR_COUNT) visible = FR_COUNT;
  if (overlayRowIndex < overlayScroll_) overlayScroll_ = overlayRowIndex;
  if (overlayRowIndex >= overlayScroll_ + visible) overlayScroll_ = overlayRowIndex - visible + 1;
  overlayRowPitch_ = pitch;
  overlayRow0Y_ = startY + headerH;

  renderer.fillRect(0, startY, screenWidth, overlayH, false);
  renderer.fillRect(0, startY, screenWidth, 1, true);
  renderer.fillRect(0, startY + overlayH - 2, screenWidth, 2, true);
  renderer.drawText(UI_12_FONT_ID, margin + 8, startY + 4, "Sort & Filter", true, EpdFontFamily::BOLD);

  auto truncated = [](const char* src) -> std::string {
    if (!src || src[0] == '\0') return "Any";
    std::string raw(src);
    if (raw.length() > 16) return raw.substr(0, 14) + "..";
    return raw;
  };
  char initialBuf[24];
  char statusBuf[16];
  int ticks = 0;
  for (int b = 0; b < 5; b++)
    if (pendingState.statusMask & (1u << b)) ticks++;
  if (pendingState.publicationMask & 0x01) ticks++;
  if (pendingState.publicationMask & 0x02) ticks++;
  if (pendingState.favouriteOnly) ticks++;
  if (ticks == 0) strncpy(statusBuf, "Any", sizeof(statusBuf));
  else snprintf(statusBuf, sizeof(statusBuf), "%d on", ticks);

  const char* sortLabel = "Title";
  switch (pendingState.sortMode) {
    case SortMode::ALPHABETIC: sortLabel = "Title"; break;
    case SortMode::AUTHOR: sortLabel = "Author"; break;
    case SortMode::WORD_COUNT: sortLabel = "Word Count"; break;
    case SortMode::DATE_ADDED: sortLabel = "Date Added"; break;
    case SortMode::SERIES: sortLabel = "Series"; break;
  }
  std::string relVal = pendingState.relationshipNoneOnly ? "None" : truncated(pendingState.relationship);
  std::string fandomVal = truncated(pendingState.fandom);
  std::string textVal = truncated(pendingState.textQuery);

  for (int row = overlayScroll_; row < overlayScroll_ + visible && row < FR_COUNT; row++) {
    const int rowY = overlayRow0Y_ + (row - overlayScroll_) * pitch;
    const bool selected = overlayRowIndex == row;
    const bool clearRow = row == FR_CLEAR_ALL || row == FR_CLEAR_FANDOM || row == FR_CLEAR_REL ||
                          row == FR_CLEAR_TITLE || row == FR_CLEAR_AUTHOR || row == FR_CLEAR_STATUS ||
                          row == FR_CLEAR_TEXT;
    const bool disabled = row == FR_REL && pendingState.fandom[0] == '\0';
    if (selected && !disabled) highlightRow(renderer, margin, rowY, screenWidth - 2 * margin, pitch - 2);

    const char* label = "";
    const char* value = nullptr;
    switch (row) {
      case FR_CLEAR_ALL: label = "Clear all"; break;
      case FR_FANDOM: label = "Fandom"; value = fandomVal.c_str(); break;
      case FR_CLEAR_FANDOM: label = "Clear"; break;
      case FR_REL: label = "Relationship"; value = relVal.c_str(); break;
      case FR_CLEAR_REL: label = "Clear"; break;
      case FR_TITLE:
        formatInitialMask(pendingState.titleInitials, initialBuf, sizeof(initialBuf));
        label = "Title starts";
        value = initialBuf;
        break;
      case FR_CLEAR_TITLE: label = "Clear"; break;
      case FR_AUTHOR:
        formatInitialMask(pendingState.authorInitials, initialBuf, sizeof(initialBuf));
        label = "Author starts";
        value = initialBuf;
        break;
      case FR_CLEAR_AUTHOR: label = "Clear"; break;
      case FR_STATUS: label = "Status"; value = statusBuf; break;
      case FR_CLEAR_STATUS: label = "Clear"; break;
      case FR_TEXT: label = "Text or tag"; value = textVal.c_str(); break;
      case FR_CLEAR_TEXT: label = "Clear"; break;
      case FR_SORT: label = "Sort by"; value = sortLabel; break;
      case FR_ORDER: label = "Order"; value = pendingState.ascending ? "Ascending" : "Descending"; break;
      case FR_APPLY: label = "Apply"; break;
      default: break;
    }
    const int textY = rowY + 2;
    if (clearRow) {
      renderer.drawText(UI_10_FONT_ID, margin + 28, textY, label, true);
    } else if (row == FR_APPLY) {
      renderer.drawText(UI_10_FONT_ID, margin + 8, textY, label, true, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(UI_10_FONT_ID, margin + 8, textY, label, true);
    }
    if (value && value[0]) {
      const int valW = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, screenWidth - margin - valW - 8, textY, value, true);
    }
    if (disabled) {
      for (int py = rowY; py < rowY + pitch - 2; py++)
        for (int px = margin; px < screenWidth - margin; px++)
          if (((px + py) & 1) == 0) renderer.drawPixel(px, py, false);
    }
  }
}

void Ao3LibraryActivity::renderLetterGrid() {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* title = letterForTitle ? "Title starts" : "Author starts";
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
  const int originY = metrics.topPadding + metrics.headerHeight + 28;
  KeyBox keys[40];
  const int n = layoutLetterKeys(renderer.getScreenWidth(), originY, keys, 40);
  const uint64_t mask = letterForTitle ? pendingState.titleInitials : pendingState.authorInitials;
  int cursor = 0;
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < letterRowCount(row); col++, cursor++) {
      if (cursor >= n) break;
      const bool on = (mask & (uint64_t{1} << initialBit(static_cast<unsigned char>(keys[cursor].ch)))) != 0;
      const bool selected = letterRow == row && letterCol == col;
      if (on) renderer.fillRect(keys[cursor].x, keys[cursor].y, keys[cursor].w, keys[cursor].h, true);
      else if (selected)
        highlightRow(renderer, keys[cursor].x, keys[cursor].y, keys[cursor].w, keys[cursor].h);
      if (!on) strokeBox(renderer, keys[cursor].x, keys[cursor].y, keys[cursor].w, keys[cursor].h);
      char label[2] = {keys[cursor].ch, 0};
      const int tw = renderer.getTextWidth(UI_12_FONT_ID, label);
      renderer.drawText(UI_12_FONT_ID, keys[cursor].x + (keys[cursor].w - tw) / 2, keys[cursor].y + 8, label, !on,
                         EpdFontFamily::BOLD);
    }
  }
  for (int i = cursor; i < n; i++) {
    const KeyBox& key = keys[i];
    const bool selected = letterRow >= 4 && ((key.clear && letterCol == 0) || (key.apply && letterCol == 1));
    if (selected) highlightRow(renderer, key.x, key.y, key.w, key.h);
    strokeBox(renderer, key.x, key.y, key.w, key.h);
    renderer.drawCenteredText(UI_10_FONT_ID, key.y + 8, key.apply ? "Apply" : "Clear");
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void Ao3LibraryActivity::renderStatusTicks() {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 "Status filter");
  static const char* labels[kStatusRows] = {
      "Combine",           "Favorite",         "Unread",
      "Reading",           "Finished Reading", "Waiting for Chapter",
      "New Chapter Available", "WIP",          "Completed in AO3",
      "Apply"};
  const int hintH = metrics.buttonHintsHeight > 0 ? metrics.buttonHintsHeight : kBottomArrowRaise;
  statusTop_ = metrics.topPadding + metrics.headerHeight + 12;
  int avail = renderer.getScreenHeight() - hintH - 8 - statusTop_;
  statusPitch_ = avail / kStatusRows;
  if (statusPitch_ > 34) statusPitch_ = 34;
  if (statusPitch_ < 26) statusPitch_ = 26;
  const int margin = 16;
  for (int i = 0; i < kStatusRows; i++) {
    const int y = statusTop_ + i * statusPitch_;
    if (i == statusRow) highlightRow(renderer, 8, y - 2, renderer.getScreenWidth() - 16, statusPitch_ - 2);
    bool on = false;
    if (i == 1) on = pendingState.favouriteOnly;
    else if (i >= 2 && i <= 6) on = (pendingState.statusMask & (1u << (i - 2))) != 0;
    else if (i == 7) on = (pendingState.publicationMask & 0x01) != 0;
    else if (i == 8) on = (pendingState.publicationMask & 0x02) != 0;
    if (i == 0) {
      renderer.drawText(UI_10_FONT_ID, margin, y, labels[i], true);
      const char* mode = pendingState.combine == CombineMode::ANY ? "Any" : "All";
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, mode);
      renderer.drawText(UI_10_FONT_ID, renderer.getScreenWidth() - margin - tw, y, mode, true, EpdFontFamily::BOLD);
    } else if (i == 9) {
      renderer.drawText(UI_10_FONT_ID, margin, y, labels[i], true, EpdFontFamily::BOLD);
    } else {
      drawTickBox(renderer, margin, y, on);
      renderer.drawText(UI_10_FONT_ID, margin + 24, y, labels[i], true);
    }
  }
  const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}

void Ao3LibraryActivity::renderPicker() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* title = (screenState == ScreenState::FANDOM_PICKER) ? "Select Fandom" : "Select Relationship";

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
  renderUi();

  // Multi-pass layout convergence for virtualized list feedback
  for (int pass = 0; pickerNav.consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
    renderUi();
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
//  renderEntry
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::renderEntry(RenderLock& lock, int y, const ViewEntry& ve, int cacheSlot, bool selected) {
  const int margin         = 20;
  const int selectionHeight = 56;
  const int squareSize     = selectionHeight;
  const int textX          = margin + squareSize + 15;

  if (selected) {
    renderer.fillRoundedRect(textX - 8, y - 3,
                             renderer.getScreenWidth() - textX - 15,
                             selectionHeight + 6, 8, LightGray);
  }

  const Ao3LibraryMetadata& meta       = pageCache[cacheSlot];
  const bool                metaLoaded = meta.isValid();

  const char rating    = metaLoaded ? meta.rating    : '-';
  const char warning   = metaLoaded ? meta.warning   : 0;
  const bool completed = metaLoaded ? (bool)meta.isCompleted : false;

  drawAo3Square(lock, margin, y, squareSize, rating, warning, completed, pageCacheStatus[cacheSlot], pageCacheMarkedPosition[cacheSlot]);

  std::string title = metaLoaded && meta.title[0] ? std::string(meta.title) : std::string(ve.title);
  std::string authorText = metaLoaded && meta.author[0]
                               ? std::string(meta.author)
                               : std::string(ve.authorKey);

  if (metaLoaded && meta.seriesName[0] != 0) {
    if (authorText.length() > 11) {
      authorText = authorText.substr(0, 11) + ".";
    }
    char seriesBuf[256];
    if (meta.seriesPart > 0) {
      sprintf(seriesBuf, " \xE2\x80\xA2 %d of %s", meta.seriesPart, meta.seriesName);
    } else {
      sprintf(seriesBuf, " \xE2\x80\xA2 %s", meta.seriesName);
    }
    authorText += seriesBuf;
  }

  const int maxTextWidth = renderer.getScreenWidth() - textX - 25;

  auto truncateToFit = [&](std::string& text, int fontId, EpdFontFamily::Style style) {
    if (renderer.getTextWidth(fontId, text.c_str(), style) > maxTextWidth) {
      while (!text.empty() &&
             renderer.getTextWidth(fontId, (text + "..").c_str(), style) > maxTextWidth) {
        while (!text.empty()) {
          const char c = text.back();
          text.pop_back();
          if ((c & 0xC0) != 0x80) break;
        }
      }
      text += "..";
    }
  };

  truncateToFit(title,      UI_12_FONT_ID, EpdFontFamily::BOLD);
  truncateToFit(authorText, UI_10_FONT_ID, EpdFontFamily::REGULAR);

  renderer.drawText(UI_12_FONT_ID, textX, y + 6,  title.c_str(),      true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, textX, y + 32, authorText.c_str());

  if (metaLoaded) {
    int blockY = y + selectionHeight + 12;

    // Tags
    int tagX = margin;
    for (int j = 0; j < 4; j++) {
      if (meta.tags[j][0] == 0) break;
      const int tagW = renderer.getTextWidth(SMALL_FONT_ID, meta.tags[j]) + 16;
      if (tagX + tagW > renderer.getScreenWidth() - margin) break;
      renderer.drawRoundedRect(tagX, blockY, tagW, 20, 1, 6, true);
      renderer.drawText(SMALL_FONT_ID, tagX + 8, blockY - 2, meta.tags[j]);
      tagX += tagW + 8;
    }
    blockY += 28;

    for (const auto& line : wrappedSummary[cacheSlot]) {
      renderer.drawText(SMALL_FONT_ID, margin, blockY, line.c_str());
      blockY += 20;
    }
    blockY += 10;

    char metaBuf[128];
    char chapters[32];
    const uint16_t planned = pageCachePlanned[cacheSlot];
    if (planned > 0) {
      snprintf(chapters, sizeof(chapters), "Chapters: %u/%u", meta.chapterCount, planned);
    } else if (meta.isCompleted && meta.chapterCount > 0) {
      snprintf(chapters, sizeof(chapters), "Chapters: %u/%u", meta.chapterCount, meta.chapterCount);
    } else {
      snprintf(chapters, sizeof(chapters), "Chapters: %u/?", meta.chapterCount);
    }
    if (meta.updatedDate[0] != '\0') {
      snprintf(metaBuf, sizeof(metaBuf), "%s   Words: %lu   Updated: %s", chapters, (unsigned long)meta.wordCount,
               meta.updatedDate);
    } else {
      snprintf(metaBuf, sizeof(metaBuf), "%s   Words: %lu", chapters, (unsigned long)meta.wordCount);
    }
    renderer.drawText(SMALL_FONT_ID, margin, blockY, metaBuf);
  }
}

// ---------------------------------------------------------------------------
//  drawAo3Square
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::drawAo3Square(RenderLock& lock, int x, int y, int s,
                                       char rating, char warning, bool completed,
                                       BookStatus status, int markedPosition) {
  const int h = s / 2;

  renderSymbol(x + 1, y + 1, h - 1, rating, true, false, false, false, -1);
  renderStatusSymbol(x + h + 1, y + 1, h - 1, status, false, true, false, false, -1, markedPosition);
  renderWarningSymbol(x + 1, y + h + 1, h - 1, warning, false, false, true, false, -2);
  renderCompletionSymbol(x + h + 1, y + h + 1, h - 1, completed, false, false, false, true, -2);

  renderer.drawRoundedRect(x, y, s, s, 1, 6, true);
  renderer.drawLine(x + 1, y + h, x + s - 1, y + h);
  renderer.drawLine(x + h, y + 1, x + h, y + s - 1);
}

// ---------------------------------------------------------------------------
//  Symbol Renderers
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::renderSymbol(int x, int y, int s, char c, bool tl, bool tr, bool bl, bool br, int yOffset) {
  Color bg = White;
  if (c == 'T') bg = LightGray;
  if (c == 'M') bg = DarkGray;
  if (c == 'E') bg = Black;
  if (bg != White) renderer.fillRoundedRect(x, y, s, s, 6, tl, tr, bl, br, bg);
  char buf[2] = {c, 0};
  if (c == '-' || c == 0) buf[0] = '-';
  const int tw = renderer.getTextWidth(UI_10_FONT_ID, buf);
  const int th = renderer.getTextHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x + (s - tw)/2, y + (s - th)/2 + yOffset,
                    buf, (bg == DarkGray || bg == Black) ? false : true);
}

void Ao3LibraryActivity::renderStatusSymbol(int x, int y, int s, BookStatus status, bool tl, bool tr, bool bl, bool br, int yOffset, int markedPosition) {
  if (status == BookStatus::MARKED_FOR_LATER) {
    if (markedPosition > 0) {
      char buf[3];
      snprintf(buf, sizeof(buf), "%d", markedPosition);
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, buf);
      const int th = renderer.getTextHeight(UI_10_FONT_ID);
      renderer.drawText(UI_10_FONT_ID, x + (s - tw) / 2 , y + (s - th) / 2 + yOffset, buf, true);
    }
    return;
  }

  // Handle geometric custom renders for chapter status updates
  if (status == BookStatus::WAITING_FOR_CHAPTER || status == BookStatus::NEW_CHAPTER_AVAILABLE) {
    // 1. Calculate an upward-pointing triangle centered inside the quadrant
    int triW = s / 2;
    int triH = s / 2;
    int triX = x + (s - triW) / 2;
    int triY = y + (s - triH) / 2 + yOffset;

    int xPts[] = { triX + triW / 2, triX, triX + triW };
    int yPts[] = { triY, triY + triH, triY + triH };

    // Draw the black filled triangle (Both statuses get this on a white background)
    renderer.fillPolygon(xPts, yPts, 3, Black);

    // 2. If a new chapter is available, add the 9x9 circle notification badge
    if (status == BookStatus::NEW_CHAPTER_AVAILABLE) {
      int dotW = 11;
      int dotH = 10;
      // Positioned with a 1-pixel margin inside the top-right corner radius padding
      int dotX = x + s - 6;
      int dotY = y - 3;

      // A corner radius of 4 on a 9x9 square forces a perfect circle primitive
      renderer.fillRoundedRect(dotX, dotY, dotW, dotH, 4, true, true, true, true, Black);
    }
    return; // Early exit so we don't fall back to font text rendering
  }

  // --- Fallback text rendering for standard statuses (READING, FINISHED, etc.) ---
  const char* txt = "-";
  Color bg = White;
  if (status == BookStatus::READING)               { bg = LightGray; txt = "R"; }
  if (status == BookStatus::FINISHED)              { bg = Black;     txt = "F"; }

  if (bg != White) renderer.fillRoundedRect(x, y, s, s, 6, tl, tr, bl, br, bg);
  const int tw = renderer.getTextWidth(UI_10_FONT_ID, txt);
  const int th = renderer.getTextHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x + (s - tw)/2, y + (s - th)/2 + yOffset,
                    txt, (bg == Black) ? false : true);
}

void Ao3LibraryActivity::renderWarningSymbol(int x, int y, int s, char warning, bool tl, bool tr, bool bl, bool br, int yOffset) {
  Color bg = White;
  const char* txt = "-";
  if (warning == 'B') { bg = DarkGray; txt = "!?"; }
  if (warning == '!') { bg = Black;    txt = "!"; }
  if (bg != White) renderer.fillRoundedRect(x, y, s, s, 6, tl, tr, bl, br, bg);
  const int tw = renderer.getTextWidth(UI_10_FONT_ID, txt);
  const int th = renderer.getTextHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x + (s - tw)/2, y + (s - th)/2 + yOffset,
                    txt, (bg == DarkGray || bg == Black) ? false : true);
}

void Ao3LibraryActivity::renderCompletionSymbol(int x, int y, int s, bool completed, bool tl, bool tr, bool bl, bool br, int yOffset) {
  const Color bg  = completed ? LightGray : Black;
  renderer.fillRoundedRect(x, y, s, s, 6, tl, tr, bl, br, bg);

  if (completed) {
    // Draw a sharp 3px thick black checkmark using line primitives
    for (int dy = 0; dy < 4; dy++) {
      // Left short downward stroke
      renderer.drawLine(x + 8, y + 14 + yOffset + dy, x + 12, y + 18 + yOffset + dy);
      // Right long upward stroke
      renderer.drawLine(x + 12, y + 18 + yOffset + dy, x + 19, y + 11 + yOffset + dy);
    }
  } else {
    // Draw a 4px thick cross (X) using white line primitives
    // The 5th parameter (4) sets the thickness, and the 6th parameter (false) forces it to draw White

    // Line 1: Top-Left to Bottom-Right
    renderer.drawLine(x + 7, y + 8 + yOffset, x + 18, y + 18 + yOffset, 4, false);

    // Line 2: Bottom-Left to Top-Right
    renderer.drawLine(x + 7, y + 18 + yOffset, x + 18, y + 8 + yOffset, 4, false);
  }
}

// ---------------------------------------------------------------------------
//  Sort & Filter States Load/Save
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::loadSortFilterState() {
  memset(activeState.fandom,       0, 32);
  memset(activeState.relationship, 0, 32);
  activeState.relationshipNoneOnly = false;
  activeState.sortMode  = SortMode::ALPHABETIC;
  activeState.ascending = true;
  activeState.titleInitials = 0;
  activeState.authorInitials = 0;
  activeState.statusMask = 0;
  activeState.publicationMask = 0;
  activeState.favouriteOnly = false;
  activeState.combine = CombineMode::ALL;
  activeState.textQuery[0] = '\0';

  const char* path = "/.crosspoint/ao3SortFilterState.json";
  if (!Storage.exists(path)) return;

  String json = Storage.readFile(path);
  if (json.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return;

  strncpy(activeState.fandom,       doc["fandom"]       | "", 31);
  strncpy(activeState.relationship, doc["relationship"] | "", 31);
  activeState.relationshipNoneOnly = doc["relationshipNoneOnly"] | false;
  activeState.sortMode  = static_cast<SortMode>(doc["sortMode"] | 0);
  activeState.ascending = doc["ascending"] | true;
  activeState.titleInitials = strtoull(doc["titleInitials"] | "0", nullptr, 16);
  activeState.authorInitials = strtoull(doc["authorInitials"] | "0", nullptr, 16);
  activeState.statusMask = doc["statusMask"] | 0;
  activeState.publicationMask = doc["publicationMask"] | 0;
  activeState.favouriteOnly = doc["favouriteOnly"] | false;
  activeState.combine = static_cast<CombineMode>(doc["combine"] | 0);
  strncpy(activeState.textQuery, doc["textQuery"] | "", 31);
  activeState.textQuery[31] = '\0';

  if (activeState.sortMode > SortMode::AUTHOR)
    activeState.sortMode = SortMode::ALPHABETIC;

  if (activeState.fandom[0] == '\0') {
    memset(activeState.relationship, 0, 32);
    activeState.relationshipNoneOnly = false;
  }

  uint8_t persistedFm = doc["filterMode"] | 0;
  if (activeState.fandom[0] != '\0' && persistedFm != static_cast<uint8_t>(filterMode)) {
      memset(activeState.fandom,       0, 32);
      memset(activeState.relationship, 0, 32);
      activeState.relationshipNoneOnly = false;
  }

  if (filterMode == FilterMode::FOLDER_TREE && ao3Folder.empty()) {
    memset(activeState.fandom,       0, 32);
    memset(activeState.relationship, 0, 32);
    activeState.relationshipNoneOnly = false;
  }
  if (filterMode == FilterMode::FOLDER_TREE) {
    folderTreeFandom       = doc["folderTreeFandom"]       | std::string(activeState.fandom);
    folderTreeRelationship = doc["folderTreeRelationship"] | std::string(activeState.relationship);
  }
}

void Ao3LibraryActivity::saveSortFilterState() const {
  JsonDocument doc;
  doc["fandom"]               = activeState.fandom;
  doc["relationship"]         = activeState.relationship;
  doc["relationshipNoneOnly"] = activeState.relationshipNoneOnly;
  doc["sortMode"]             = static_cast<uint8_t>(activeState.sortMode);
  doc["ascending"]            = activeState.ascending;
  char titleMask[20];
  char authorMask[20];
  snprintf(titleMask, sizeof(titleMask), "%llx", static_cast<unsigned long long>(activeState.titleInitials));
  snprintf(authorMask, sizeof(authorMask), "%llx", static_cast<unsigned long long>(activeState.authorInitials));
  doc["titleInitials"] = titleMask;
  doc["authorInitials"] = authorMask;
  doc["statusMask"] = activeState.statusMask;
  doc["publicationMask"] = activeState.publicationMask;
  doc["favouriteOnly"] = activeState.favouriteOnly;
  doc["combine"] = static_cast<uint8_t>(activeState.combine);
  doc["textQuery"] = activeState.textQuery;
  doc["filterMode"]           = static_cast<uint8_t>(filterMode);
  if (filterMode == FilterMode::FOLDER_TREE) {
    doc["folderTreeFandom"]       = folderTreeFandom;
    doc["folderTreeRelationship"] = folderTreeRelationship;
  }

  String json;
  serializeJson(doc, json);
  Storage.writeFile("/.crosspoint/ao3SortFilterState.json", json);
}

// ---------------------------------------------------------------------------
//  Sorting Logic
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::resortViewEntries() {
  switch (activeState.sortMode) {

    case SortMode::ALPHABETIC:
      std::sort(viewEntries.begin(), viewEntries.end(), [&](const ViewEntry& a, const ViewEntry& b) {
        int cmp = strncasecmp(a.title, b.title, 12);
      return activeState.ascending ? cmp < 0 : cmp > 0;
      });
      break;

    case SortMode::WORD_COUNT:
      std::sort(viewEntries.begin(), viewEntries.end(), [&](const ViewEntry& a, const ViewEntry& b) {
        if (a.wordCount != b.wordCount)
          return activeState.ascending ? a.wordCount < b.wordCount : a.wordCount > b.wordCount;
        return strncmp(a.title, b.title, 12) < 0;
      });
      break;

    case SortMode::DATE_ADDED:
      std::sort(viewEntries.begin(), viewEntries.end(), [&](const ViewEntry& a, const ViewEntry& b) {
        if (a.addedSequence != b.addedSequence)
          return activeState.ascending ? a.addedSequence < b.addedSequence : a.addedSequence > b.addedSequence;
        return strncmp(a.title, b.title, 12) < 0;
      });
      break;

    case SortMode::SERIES:
      std::sort(viewEntries.begin(), viewEntries.end(), [&](const ViewEntry& a, const ViewEntry& b) {
        bool aHas = a.seriesHash != 0;
        bool bHas = b.seriesHash != 0;
        if (aHas != bHas) return aHas > bHas; // no-series goes last

        if (a.seriesHash != b.seriesHash)
          return activeState.ascending ? a.seriesHash < b.seriesHash : a.seriesHash > b.seriesHash;

        if (a.seriesPart != b.seriesPart)
          return a.seriesPart < b.seriesPart; // always ascending seriesPart

        return strncmp(a.title, b.title, 12) < 0;
      });
      break;

    case SortMode::AUTHOR:
      std::sort(viewEntries.begin(), viewEntries.end(), [&](const ViewEntry& a, const ViewEntry& b) {
        int cmp = strncmp(a.authorKey, b.authorKey, 8);
        if (cmp != 0)
          return activeState.ascending ? cmp < 0 : cmp > 0;
        return strncmp(a.title, b.title, 12) < 0;
      });
      break;
  }
}

// ---------------------------------------------------------------------------
//  Filtering Logic
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::commitPendingFilters() {
  SortFilterState previous = activeState;
  activeState = pendingState;
  screenState = ScreenState::LIBRARY;
  saveSortFilterState();
  applyStateChange(previous, activeState);
  requestUpdate(true);
}

void Ao3LibraryActivity::clearFandomFilter() {
  pendingState.fandom[0] = '\0';
  pendingState.relationship[0] = '\0';
  pendingState.relationshipNoneOnly = false;
  folderTreeFandom.clear();
  folderTreeRelationship.clear();
}

void Ao3LibraryActivity::clearAllFilters() {
  clearFandomFilter();
  pendingState.titleInitials = 0;
  pendingState.authorInitials = 0;
  pendingState.statusMask = 0;
  pendingState.publicationMask = 0;
  pendingState.favouriteOnly = false;
  pendingState.combine = CombineMode::ALL;
  pendingState.textQuery[0] = '\0';
}

bool Ao3LibraryActivity::passesFilter(const ViewEntry& v, const FilterHashes& h) const {
  if (filterMode == FilterMode::FOLDER_TREE) {
      if (!std::binary_search(allowedHashes.begin(), allowedHashes.end(), v.cacheHash)) return false;
  } else if (h.fandomActive) {
    if (Ao3TagMergeStore::resolveFandom(v.fandomHash) != h.fandomHash) return false;
    if (h.relationshipNoneOnly) {
      if (v.rel1Hash != 0 || v.rel2Hash != 0) return false;
    } else if (h.relationshipActive) {
      if (Ao3TagMergeStore::resolveRelationship(v.rel1Hash) != h.relationshipHash &&
        Ao3TagMergeStore::resolveRelationship(v.rel2Hash) != h.relationshipHash) return false;
    }
  }

  if (!initialPasses(activeState.titleInitials, static_cast<unsigned char>(v.title[0]))) return false;
  if (!initialPasses(activeState.authorInitials, static_cast<unsigned char>(v.authorKey[0]))) return false;

  const bool statusOn = activeState.statusMask != 0;
  const bool pubOn = activeState.publicationMask != 0;
  const bool favOn = activeState.favouriteOnly;
  if (statusOn || pubOn || favOn) {
    const uint8_t status = v.filterBits & kFilterStatusMask;
    const bool statusHit = statusOn && status <= 4 && (activeState.statusMask & (1u << status)) != 0;
    const bool completed = (v.filterBits & kFilterMetaCompleted) != 0;
    const bool pubHit = (activeState.publicationMask & 0x01 && !completed) ||
                        (activeState.publicationMask & 0x02 && completed);
    const bool favHit = favOn && (v.filterBits & kFilterFavourite) != 0;
    if (activeState.combine == CombineMode::ANY) {
      if (!statusHit && !pubHit && !favHit) return false;
    } else {
      if (statusOn && !statusHit) return false;
      if (pubOn && !pubHit) return false;
      if (favOn && !favHit) return false;
    }
  }
  return true;
}

bool Ao3LibraryActivity::sidecarTagsMatch(uint32_t cacheHash, const char* query) const {
  if (!query || query[0] == '\0') return true;
  const std::string path = "/.crosspoint/epub_" + std::to_string(cacheHash) + "/ao3_library_info";
  HalFile f;
  if (!Storage.openFileForRead("AO3L", path, f)) return false;
  // tags[4][16] sits at byte 656 of Ao3LibraryMetadata. See sanitizeIndex.
  if (!f.seek(656)) {
    f.close();
    return false;
  }
  char tag[17];
  bool matched = false;
  for (int i = 0; i < 4 && !matched; i++) {
    memset(tag, 0, sizeof(tag));
    f.read(reinterpret_cast<uint8_t*>(tag), 16);
    tag[16] = '\0';
    if (tag[0] && containsFold(tag, query)) matched = true;
  }
  f.close();
  return matched;
}

void Ao3LibraryActivity::rebuildViewEntries() {
  viewEntries.clear();
  const char* indexPath = "/.crosspoint/ao3_library_index.bin";

  if (!Storage.exists(indexPath)) {
    indexState = IndexState::MISSING;
    return;
  }

  HalFile f;
  if (!Storage.openFileForRead("AO3L", indexPath, f)) {
    indexState = IndexState::CORRUPT;
    return;
  }

  char     magic[4];
  uint8_t  version;
  uint16_t recordCount;
  uint32_t nextSequence;
  uint8_t  reserved;

  const bool readOk =
      f.read(magic, 4) == 4 &&
      f.read(&version, 1) == 1 &&
      f.read((uint8_t*)&recordCount, 2) == 2 &&
      f.read((uint8_t*)&nextSequence, 4) == 4 &&
      f.read(&reserved, 1) == 1;

  if (!readOk || memcmp(magic, "AO3X", 4) != 0 || version != 1) {
    f.close();
    indexState = IndexState::CORRUPT;
    return;
  }

  if (recordCount > MAX_LIBRARY_BOOKS) {
    f.close();
    indexState = IndexState::CORRUPT;
    return;
  }

  if (filterMode == FilterMode::FOLDER_TREE && allowedHashes.empty() && !ao3Folder.empty()) {
      if (activeState.fandom[0] != '\0') {
          std::string scanPath = ao3Folder;
          if (scanPath.back() != '/') scanPath += "/";
          scanPath += folderTreeFandom;
          if (activeState.relationship[0] != '\0') {
              scanPath += "/";
              scanPath += folderTreeRelationship;
              buildAllowedHashes(scanPath, 0);
          } else {
              buildAllowedHashes(scanPath, 1);
          }
      } else {
          buildAllowedHashes(ao3Folder, 2);
      }
  }

  const FilterHashes filterHashes = computeFilterHashes(activeState);
  viewEntries.reserve(recordCount);

  bool hideFinished = false;
  {
    const char* settingsPath = "/.crosspoint/ao3_settings.json";
    if (Storage.exists(settingsPath)) {
      String json = Storage.readFile(settingsPath);
      if (!json.isEmpty()) {
        JsonDocument doc;
        if (!deserializeJson(doc, json)) {
          hideFinished = doc["hideFinished"] | false;
        }
      }
    }
  }

  CompactIndexRecord rec;
  for (uint16_t i = 0; i < recordCount; i++) {
    if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
    if (rec.flags & 0x01) continue;           // Skip tombstone
    if (hideFinished && (rec.flags & 0x02)) continue; // Skip finished
    if (activeState.textQuery[0] != '\0' && !compactTextMatch(rec, activeState.textQuery) &&
        !sidecarTagsMatch(rec.cacheHash, activeState.textQuery)) {
      yield();
      continue;
    }
    ViewEntry v = buildViewEntry(rec);
    if (activeState.statusMask != 0) {
      const auto st = static_cast<uint8_t>(getBookStatus(rec.cacheHash));
      v.filterBits = static_cast<uint8_t>((v.filterBits & ~kFilterStatusMask) | (st & kFilterStatusMask));
    }
    if (passesFilter(v, filterHashes)) {
      viewEntries.push_back(v);
    }
    yield();
  }
  f.close();

  resortViewEntries();
  indexState = IndexState::OK;
  cachedPage = -1;
  buttonsSetup = false;

  // A restored index (silent-restart resume) may exceed the filtered list
  if (selectorIndex >= viewEntries.size())
    selectorIndex = viewEntries.empty() ? 0 : viewEntries.size() - 1;
}

void Ao3LibraryActivity::applyStateChange(const SortFilterState& prev, const SortFilterState& next) {
  bool filterChanged = filterFieldsDiffer(prev, next);

  bool sortChanged =
      prev.sortMode  != next.sortMode ||
      prev.ascending != next.ascending;

  const bool treeChanged = strcmp(prev.fandom, next.fandom) != 0 ||
                           strcmp(prev.relationship, next.relationship) != 0 ||
                           prev.relationshipNoneOnly != next.relationshipNoneOnly;

  if (filterMode == FilterMode::FOLDER_TREE && treeChanged) {
      allowedHashes.clear();
      if (next.fandom[0] != '\0') {
          std::string scanPath = ao3Folder;
          if (scanPath.back() != '/') scanPath += "/";
          scanPath += folderTreeFandom;
          if (next.relationship[0] != '\0') {
              scanPath += "/";
              scanPath += folderTreeRelationship;
              buildAllowedHashes(scanPath, 0);
          } else {
              buildAllowedHashes(scanPath, 1);
          }
      } else {
          if (!ao3Folder.empty()) buildAllowedHashes(ao3Folder, 2);
      }
  }

  if (filterChanged) {
    rebuildViewEntries();
  } else if (sortChanged) {
    resortViewEntries();
  }

  selectorIndex = 0;
  cachedPage    = -1;
  requestUpdate();
}

// ---------------------------------------------------------------------------
//  Picker List Builders
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::buildFandomList(std::vector<std::string>& out) const {
  if (filterMode == FilterMode::FOLDER_TREE) {
      if (ao3Folder.empty()) return;
      HalFile root = Storage.open(ao3Folder.c_str());
      if (!root || !root.isDirectory()) { if (root) root.close(); return; }

      char name[256];
      HalFile entry;
      while (entry = root.openNextFile()) {
          entry.getName(name, sizeof(name));
          if (entry.isDirectory() && name[0] != '.' &&
              strcmp(name, "System Volume Information") != 0) {
              out.push_back(name);
          }
          entry.close();
      }
      root.close();
      std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
          return strcasecmp(a.c_str(), b.c_str()) < 0;
      });
      return;
  }

  const char* indexPath = "/.crosspoint/ao3_library_index.bin";
  HalFile f;
  if (!Storage.openFileForRead("AO3L", indexPath, f)) return;

  char     magic[4];
  uint8_t  version;
  uint16_t recordCount;
  f.read(magic, 4);
  f.read(&version, 1);
  f.read((uint8_t*)&recordCount, 2);

  if (memcmp(magic, "AO3X", 4) != 0 || recordCount > MAX_LIBRARY_BOOKS) {
    f.close();
    return;
  }

  // Skip remaining header bytes (nextSequence [4] + reserved [1] = 5 bytes)
  f.seek(12);

  CompactIndexRecord rec;
  for (uint16_t i = 0; i < recordCount; i++) {
    if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
    if (rec.flags & 0x01 || rec.fandom[0] == '\0') continue;
    if (Ao3TagMergeStore::isSubFandom(fnv1a(rec.fandom))) continue;

    bool found = false;
    for (const auto& s : out) {
      if (s == rec.fandom) {
        found = true;
        break;
      }
    }
    if (!found) {
      out.push_back(rec.fandom);
    }
  }
  f.close();
  std::sort(out.begin(), out.end());
}

void Ao3LibraryActivity::buildRelationshipList(const char* fandom, std::vector<std::string>& out, bool& hasNoneEntries) const {
  hasNoneEntries = false;

  if (filterMode == FilterMode::FOLDER_TREE) {
      if (ao3Folder.empty() || !fandom || fandom[0] == '\0') return;

      std::string fandomPath = ao3Folder;
      if (fandomPath.back() != '/') fandomPath += "/";
      fandomPath += fandom;

      HalFile root = Storage.open(fandomPath.c_str());
      if (!root || !root.isDirectory()) { if (root) root.close(); return; }

      char name[256];
      HalFile entry;
      while (entry = root.openNextFile()) {
          entry.getName(name, sizeof(name));
          if (entry.isDirectory() && name[0] != '.' &&
              strcmp(name, "System Volume Information") != 0) {
              out.push_back(name);
          }
          entry.close();
      }
      root.close();
      std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
          return strcasecmp(a.c_str(), b.c_str()) < 0;
      });
      return;
  }

  const char* indexPath = "/.crosspoint/ao3_library_index.bin";
  HalFile f;
  if (!Storage.openFileForRead("AO3L", indexPath, f)) return;

  char     magic[4];
  uint8_t  version;
  uint16_t recordCount;
  f.read(magic, 4);
  f.read(&version, 1);
  f.read((uint8_t*)&recordCount, 2);

  if (memcmp(magic, "AO3X", 4) != 0 || recordCount > MAX_LIBRARY_BOOKS) {
    f.close();
    return;
  }

  f.seek(12);

  CompactIndexRecord rec;
  for (uint16_t i = 0; i < recordCount; i++) {
    if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
    if (rec.flags & 0x01) continue;
    if (strcmp(rec.fandom, fandom) != 0) continue;

    if (rec.relationship1[0] == '\0' && rec.relationship2[0] == '\0') {
      hasNoneEntries = true;
      continue;
    }

    for (const char* rel : { rec.relationship1, rec.relationship2 }) {
      if (rel[0] == '\0') continue;
      if (Ao3TagMergeStore::isSubRelationship(fnv1a(rel))) continue;
      bool found = false;
      for (const auto& s : out) {
        if (s == rel) {
          found = true;
          break;
        }
      }
      if (!found) {
        out.push_back(rel);
      }
    }
  }
  f.close();
  std::sort(out.begin(), out.end());
}

// ---------------------------------------------------------------------------
//  FreeInkUI Picker Adaptations
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::pickerScreenTrampoline(UiScreen& screen, void* user) {
  static_cast<Ao3LibraryActivity*>(user)->buildPickerScreen(screen);
}

void Ao3LibraryActivity::onPickerRowActionTrampoline(const freeink::ui::ActionEvent& event, void* user) {
  if (event.action == ACTION_PICKER_ROW) {
    static_cast<Ao3LibraryActivity*>(user)->confirmPickerSelection(event.value);
  }
}

void Ao3LibraryActivity::launchPicker(ScreenState newState, int targetSelectedIndex) {
  pickerRowItems.clear();
  pickerRowItems.resize(pickerItems.size());
  for (size_t i = 0; i < pickerItems.size(); ++i) {
    pickerRowItems[i].label = pickerItems[i].c_str();
    pickerRowItems[i].actionValue = static_cast<int16_t>(i);
  }
  
  pickerNav.reset();
  pickerNav.selected = targetSelectedIndex;
  pickerNav.follow(pickerItems.size());

  resetUi();
  app.on(ACTION_PICKER_ROW, &Ao3LibraryActivity::onPickerRowActionTrampoline, this);
  app.setScreen(&Ao3LibraryActivity::pickerScreenTrampoline, this);

  screenState = newState;
}

void Ao3LibraryActivity::confirmPickerSelection(int selectedIndex) {
  if (screenState == ScreenState::FANDOM_PICKER) {
    if (selectedIndex == 0) {
      pendingState.fandom[0] = '\0';
      pendingState.relationship[0] = '\0';
      pendingState.relationshipNoneOnly = false;
      if (filterMode == FilterMode::FOLDER_TREE) {
        folderTreeFandom.clear();
        folderTreeRelationship.clear();
      }
    } else {
      strncpy(pendingState.fandom, pickerItems[selectedIndex].c_str(), 31);
      pendingState.fandom[31] = '\0';
      pendingState.relationship[0] = '\0';
      pendingState.relationshipNoneOnly = false;
      if (filterMode == FilterMode::FOLDER_TREE) {
        folderTreeFandom = pickerItems[selectedIndex];
        folderTreeRelationship.clear();
      }
    }
    overlayRowIndex = (pendingState.fandom[0] != '\0') ? FR_REL : FR_TITLE;
  } else { // RELATIONSHIP_PICKER
    if (selectedIndex == 0) {
      pendingState.relationship[0] = '\0';
      pendingState.relationshipNoneOnly = false;
      if (filterMode == FilterMode::FOLDER_TREE) folderTreeRelationship.clear();
    } else if (selectedIndex == 1 && pickerHasNone) {
      pendingState.relationship[0] = '\0';
      pendingState.relationshipNoneOnly = true;
      if (filterMode == FilterMode::FOLDER_TREE) folderTreeRelationship.clear();
    } else {
      strncpy(pendingState.relationship, pickerItems[selectedIndex].c_str(), 31);
      pendingState.relationship[31] = '\0';
      pendingState.relationshipNoneOnly = false;
      if (filterMode == FilterMode::FOLDER_TREE)
        folderTreeRelationship = pickerItems[selectedIndex];
    }
    overlayRowIndex = FR_TITLE;
  }
  closeRouting();
  pickerRowItems.clear();
  screenState = ScreenState::FILTER_PANEL;
  requestUpdate(true);
}

void Ao3LibraryActivity::buildPickerScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(freeink::ui::Insets{
      static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing),
      0,
      static_cast<int16_t>(metrics.buttonHintsHeight),
      0});

  freeink::ui::ListProps props;
  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    rowHeight = static_cast<int16_t>(metrics.listRowHeight);
    props.rowHeight = rowHeight;
  }

  pickerNav.syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, pickerItems.size(), props);

  props.action = ACTION_PICKER_ROW;
  props.inputMask = freeink::ui::InputTouch;
  
  props.labelText = screen.theme().bodyText;
  
  props.items = pickerRowItems.data();
  props.count = static_cast<uint16_t>(pickerRowItems.size());
  
  freeink::ui::StyleSet rowStyles = freeink::ui::plainStyles(freeink::ui::Paint::solid(freeink::ui::Color::Black));
  rowStyles.selected.background = freeink::ui::Paint::dither(freeink::ui::Color::LightGray);
  rowStyles.selected.foreground = freeink::ui::Paint::solid(freeink::ui::Color::Black);
  rowStyles.active = rowStyles.selected;
  props.rowStyles = rowStyles;

  screen.list(props);
}