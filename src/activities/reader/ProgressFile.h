#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace ProgressFile {

// Writes `len` bytes of reader progress to `<cachePath>/progress.bin` without
// ever leaving the canonical file half-written.
//
// The bytes go to a temporary `progress.bin.tmp` first; only once that is fully
// written and closed is it renamed over progress.bin. An interrupted write
// (power loss or a crash mid-SPI) therefore damages only the throwaway temp file.
// Previously a truncate-in-place write that was cut short left progress.bin with
// a broken FAT cluster chain that the firmware could neither rewrite nor clear,
// stranding the book on an old page (issue #2275).
//
// This is crash-safe, not metadata-atomic: on FAT the replace is remove + rename,
// two separate directory operations, so a crash between them can leave neither
// file -- which simply reads as "no saved progress" on next launch, never a
// corrupt or unclearable file. The point is that progress.bin is never torn.
//
// Note: this prevents corruption on a healthy card going forward. It cannot
// repair an already-corrupted progress.bin -- removing the stale file may itself
// fail at the FAT level, in which case recovery still requires fsck on a host.
//
// Returns true only if the new progress.bin is fully in place.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  const std::string finalPath = cachePath + "/progress.bin";
  const std::string tmpPath = cachePath + "/progress.bin.tmp";

  {
    HalFile f;
    if (!Storage.openFileForWrite("PRG", tmpPath, f)) {
      LOG_ERR("PRG", "Could not open temp progress file for write: %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(data, len);
    if (written != len) {
      LOG_ERR("PRG", "Short write saving progress to %s: %u/%u bytes", tmpPath.c_str(), (unsigned)written,
              (unsigned)len);
      return false;
    }
    f.flush();
    // f (the temp file) is closed at scope exit (DESTRUCTOR_CLOSES_FILE=1) before
    // the rename below -- SdFat must not rename a path that still has an open FsFile.
  }

  // SdFat's rename does not overwrite an existing destination, so drop the old
  // canonical file first. The brief window where neither file exists reads as
  // "no saved progress" on next launch -- never a corrupt, unclearable file.
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("PRG", "Failed to rename temp progress into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

// Byte 11 of progress.bin. Bit 0 is the favourite mark. A file shorter than
// 12 bytes has no stored mark; the index is still the source until one is written.
constexpr uint8_t kProgressFavouriteBit = 0x01;

inline int readBytes(const std::string& cachePath, uint8_t* data, size_t cap) {
  HalFile f;
  if (!Storage.openFileForRead("PRG", cachePath + "/progress.bin", f)) return 0;
  const int n = f.read(data, cap);
  f.close();
  return n < 0 ? 0 : n;
}

inline bool readFavourite(const std::string& cachePath) {
  uint8_t data[12] = {};
  const int n = readBytes(cachePath, data, sizeof(data));
  return n >= 12 && (data[11] & kProgressFavouriteBit) != 0;
}

// Writes the favourite mark without dropping the reading position or status.
// A 7-byte progress file is stored in the 12-byte layout, with its status moved
// from byte 6 to byte 10.
inline bool setFavourite(const std::string& cachePath, bool on) {
  uint8_t data[12] = {};
  const int n = readBytes(cachePath, data, sizeof(data));
  if (n == 7) {
    const uint8_t status = data[6];
    data[6] = data[7] = data[8] = data[9] = 0;
    data[10] = status;
  }
  if (on) data[11] = static_cast<uint8_t>(data[11] | kProgressFavouriteBit);
  else data[11] = static_cast<uint8_t>(data[11] & ~kProgressFavouriteBit);
  return writeAtomic(cachePath, data, sizeof(data));
}

// Rewrites the first `len` bytes (7 or 11). Keeps byte 11 when the file already has one,
// and does not add one when it does not. Adding a zero byte would look like "not a favourite".
inline bool writePreservingFavourite(const std::string& cachePath, const uint8_t* fresh, size_t len) {
  uint8_t old[12] = {};
  const int n = readBytes(cachePath, old, sizeof(old));
  if (n >= 12 && len >= 11 && len <= 11) {
    uint8_t data[12];
    memcpy(data, fresh, 11);
    data[11] = old[11];
    return writeAtomic(cachePath, data, sizeof(data));
  }
  return writeAtomic(cachePath, fresh, len);
}

}  // namespace ProgressFile
