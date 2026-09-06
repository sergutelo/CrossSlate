#include "file_manager.h"
#include "text_editor.h"
#include <Arduino.h>
#include <SDCardManager.h>
#include <cstring>

// --- File list ---
static FileInfo fileList[MAX_FILES];
static int fileCount = 0;

// Shared state
extern UIState currentState;

static bool endsWith(const char* text, const char* suffix) {
  const size_t textLen = strlen(text);
  const size_t suffixLen = strlen(suffix);
  return textLen >= suffixLen && strcmp(text + textLen - suffixLen, suffix) == 0;
}

// Recover interrupted safe-write rotations. A valid original always wins; when
// it is absent, prefer the previous .bak over a possibly partial .tmp.
static void recoverInterruptedWrites() {
  auto root = SdMan.open("/notes");
  if (!root || !root.isDirectory()) return;
  char name[MAX_FILENAME_LEN + 8];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));
    entry.close();
    if (!endsWith(name, ".tmp") && !endsWith(name, ".bak")) continue;

    const size_t nameLen = strlen(name);
    if (nameLen <= 4 || nameLen >= sizeof(name)) continue;
    char baseName[MAX_FILENAME_LEN + 8];
    memcpy(baseName, name, nameLen - 4);
    baseName[nameLen - 4] = '\0';
    char original[MAX_FILENAME_LEN + 16], tmp[MAX_FILENAME_LEN + 16], bak[MAX_FILENAME_LEN + 16];
    snprintf(original, sizeof(original), "/notes/%s", baseName);
    snprintf(tmp, sizeof(tmp), "%s.tmp", original);
    snprintf(bak, sizeof(bak), "%s.bak", original);

    if (SdMan.exists(original)) {
      if (SdMan.exists(tmp) && !SdMan.remove(tmp)) {
        DBG_PRINTF("Recovery: cannot remove %s\n", tmp);
      }
      continue;
    }
    if (SdMan.exists(bak)) {
      if (!SdMan.rename(bak, original)) {
        DBG_PRINTF("Recovery: cannot restore %s\n", bak);
      }
      if (SdMan.exists(tmp) && !SdMan.remove(tmp)) {
        DBG_PRINTF("Recovery: cannot remove %s\n", tmp);
      }
    } else if (SdMan.exists(tmp)) {
      if (!SdMan.rename(tmp, original)) {
        DBG_PRINTF("Recovery: cannot promote %s\n", tmp);
      }
    }
  }
  root.close();
}

// Convert filename to a readable display title.
// "my_note_2.txt" -> "My Note 2"
static void filenameToTitle(const char* filename, char* out, int maxLen) {
  int j = 0;
  bool capitalizeNext = true;
  for (int i = 0; filename[i] != '\0' && filename[i] != '.' && j < maxLen - 1; i++) {
    char c = filename[i];
    if (c == '_') {
      if (j > 0) out[j++] = ' ';
      capitalizeNext = true;
    } else {
      if (capitalizeNext && c >= 'a' && c <= 'z') c -= 32;
      capitalizeNext = false;
      out[j++] = c;
    }
  }
  out[j] = '\0';
  if (j == 0) strncpy(out, "Untitled", maxLen - 1);
}

// Convert a title to a valid FAT filename (lowercase, spaces->underscores,
// non-alphanumeric stripped, ".txt" appended).
static void titleToFilename(const char* title, char* out, int maxLen) {
  int maxBase = maxLen - 5; // room for ".txt" + null
  int j = 0;
  for (int i = 0; title[i] != '\0' && j < maxBase; i++) {
    char c = title[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
    } else if (c == ' ' || c == '_' || c == '-') {
      if (j > 0 && out[j - 1] != '_') out[j++] = '_';
    }
  }
  while (j > 0 && out[j - 1] == '_') j--;
  if (j == 0) { strncpy(out, "note", maxLen - 1); j = 4; }
  strcpy(out + j, ".txt");
}

// Derive a unique /notes/ filename from a title, handling collisions with _2, _3 suffix.
void deriveUniqueFilename(const char* title, char* out, int maxLen) {
  titleToFilename(title, out, maxLen);

  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", out);
  if (!SdMan.exists(path)) return;

  // Collision — strip .txt, try _2, _3 ...
  char base[MAX_FILENAME_LEN];
  strncpy(base, out, maxLen - 1);
  base[strlen(base) - 4] = '\0';

  int suffix = 2;
  while (SdMan.exists(path) && suffix <= 99) {
    snprintf(out, maxLen, "%s_%d.txt", base, suffix++);
    snprintf(path, sizeof(path), "/notes/%s", out);
  }
}

void fileManagerSetup() {
  if (!SdMan.begin()) {
    DBG_PRINTLN("SD Card mount failed!");
    return;
  }

  if (!SdMan.exists("/notes")) {
    if (!SdMan.mkdir("/notes")) {
      DBG_PRINTLN("Could not create /notes");
      return;
    }
  }

  recoverInterruptedWrites();
  DBG_PRINTLN("SD Card initialized");
  refreshFileList();
}

void refreshFileList() {
  fileCount = 0;

  auto root = SdMan.open("/notes");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();
  char name[256];

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || fileCount >= MAX_FILES) {
      file.close();
      if (fileCount >= MAX_FILES) break;
      continue;
    }

    int nameLen = strlen(name);
    if (nameLen > 4 && strcasecmp(name + nameLen - 4, ".txt") == 0) {
      strncpy(fileList[fileCount].filename, name, MAX_FILENAME_LEN - 1);
      fileList[fileCount].filename[MAX_FILENAME_LEN - 1] = '\0';

      filenameToTitle(name, fileList[fileCount].title, MAX_TITLE_LEN);
      fileList[fileCount].modTime = 0;
      fileCount++;
    }
    file.close();
  }
  root.close();
  SdMan.sleep();

  DBG_PRINTF("File listing: %d files found\n", fileCount);
}

int getFileCount() { return fileCount; }
FileInfo* getFileList() { return fileList; }

void loadFile(const char* filename) {
  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", filename);

  auto file = SdMan.open(path, O_RDONLY);
  if (!file) {
    DBG_PRINTF("Could not open: %s\n", path);
    return;
  }

  char* buf = editorGetBuffer();
  int readResult = file.read(buf, TEXT_BUFFER_SIZE - 1);
  size_t bytesRead = (readResult > 0) ? (size_t)readResult : 0;
  buf[bytesRead] = '\0';
  file.close();

  editorSetCurrentFile(filename);
  editorLoadBuffer(bytesRead);

  // Title comes from the filename, not the file content
  char title[MAX_TITLE_LEN];
  filenameToTitle(filename, title, MAX_TITLE_LEN);
  editorSetCurrentTitle(title);
  editorSetUnsavedChanges(false);

  currentState = UIState::TEXT_EDITOR;
  SdMan.sleep();
  DBG_PRINTF("Loaded: %s (%d bytes)\n", filename, (int)bytesRead);
}

bool saveCurrentFile(bool refreshList) {
  const char* filename = editorGetCurrentFile();
  if (filename[0] == '\0') return false;

  char path[96], tmpPath[104], bakPath[104];
  snprintf(path, sizeof(path), "/notes/%s", filename);
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path);

  auto file = SdMan.open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    DBG_PRINTF("saveCurrentFile: could not create tmp: %s\n", tmpPath);
    return false;
  }

  const size_t toWrite = editorGetLength();
  const size_t written = file.write((const uint8_t*)editorGetBuffer(), toWrite);
  file.close();
  if (written != toWrite) {
    DBG_PRINTF("saveCurrentFile: write mismatch (%d/%d)\n", (int)written, (int)toWrite);
    SdMan.remove(tmpPath);
    return false;
  }

  if (SdMan.exists(path)) {
    if (SdMan.exists(bakPath) && !SdMan.remove(bakPath)) {
      DBG_PRINTF("saveCurrentFile: cannot remove backup: %s\n", bakPath);
      SdMan.remove(tmpPath);
      return false;
    }
    if (!SdMan.rename(path, bakPath)) {
      DBG_PRINTF("saveCurrentFile: cannot rotate original: %s\n", path);
      SdMan.remove(tmpPath);
      return false;
    }
  }

  if (!SdMan.rename(tmpPath, path)) {
    DBG_PRINTF("saveCurrentFile: cannot promote tmp: %s\n", tmpPath);
    if (!SdMan.exists(path) && SdMan.exists(bakPath) && !SdMan.rename(bakPath, path)) {
      DBG_PRINTF("saveCurrentFile: rollback failed: %s\n", bakPath);
    }
    return false;
  }

  auto verify = SdMan.open(path, O_RDONLY);
  const bool verified = verify && verify.size() == toWrite;
  if (verify) verify.close();
  if (!verified) {
    DBG_PRINTF("saveCurrentFile: final verification failed: %s\n", path);
    SdMan.remove(path);
    if (SdMan.exists(bakPath)) SdMan.rename(bakPath, path);
    return false;
  }

  editorSetUnsavedChanges(false);
  if (refreshList) refreshFileList();
  SdMan.sleep();
  DBG_PRINTF("Saved: %s\n", filename);
  return true;
}

void createNewFile() {
  editorClear();
  editorSetCurrentFile("");       // filename derived from title when user confirms
  editorSetCurrentTitle("Untitled");
  editorSetUnsavedChanges(true);
}

// Rename a file on disk to match a new title, updating editor state if needed.
void updateFileTitle(const char* filename, const char* newTitle) {
  char newFilename[MAX_FILENAME_LEN];
  deriveUniqueFilename(newTitle, newFilename, MAX_FILENAME_LEN);

  if (strcmp(newFilename, filename) != 0) {
    char oldPath[320], newPath[320];
    snprintf(oldPath, sizeof(oldPath), "/notes/%s", filename);
    snprintf(newPath, sizeof(newPath), "/notes/%s", newFilename);
    if (!SdMan.rename(oldPath, newPath)) {
      DBG_PRINTF("Rename failed: %s -> %s\n", oldPath, newPath);
      return;
    }

    if (strcmp(editorGetCurrentFile(), filename) == 0) {
      editorSetCurrentFile(newFilename);
    }
  }

  refreshFileList();
  SdMan.sleep();
}

void deleteFile(const char* filename) {
  char path[320], bakPath[336];
  snprintf(path, sizeof(path), "/notes/%s", filename);
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
  SdMan.remove(path);
  SdMan.remove(bakPath);
  refreshFileList();
  SdMan.sleep();
  DBG_PRINTF("Deleted: %s\n", filename);
}
