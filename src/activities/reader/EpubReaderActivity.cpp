#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "fontIds.h"
#ifdef PHASE2_REMOTE_DEBUG
#include "RemoteReaderController.h"
#endif
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

#ifdef PHASE2_REMOTE_DEBUG
  // Tear down any active remote session (Wi-Fi + WebSocket) before leaving.
  if (remote_) {
    remote_->stop();
    remote_.reset();
  }
#endif

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

#if defined(PHASE2_REMOTE_DEBUG)
  // A held failure/status message stays up until any button dismisses it -> book.
  if (remoteAwaitDismiss_) {
    using B = MappedInputManager::Button;
    if (mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::Left) ||
        mappedInput.wasReleased(B::Right) || mappedInput.wasReleased(B::Confirm) || mappedInput.wasReleased(B::Back)) {
      remoteAwaitDismiss_ = false;
      requestUpdate(true);  // return to the book
    }
    return;
  }
  using B = MappedInputManager::Button;
  const bool sessionActive = remote_ && remote_->isActive();
  if (!sessionActive) {
    // No session: Volume Up starts one. Fire on PRESS, return so it preempts
    // detectPageTurn(). (Volume Down + everything else: normal reader behavior.)
    if (mappedInput.wasPressed(B::Up)) {
      toggleRemoteSession();
      return;
    }
  } else {
    // Phase 4: while a session is active the Volume rocker is the TTS remote,
    // forwarded to the phone as `button` events. Left/Right still page (and trigger
    // local authority). We track each button's OWN press time — getHeldTime() is a
    // single global timer that misreads rocker overlap (a tap could read as a long
    // press and tear down the session).
    constexpr unsigned long REMOTE_BTN_LONG_MS = 500;
    if (mappedInput.wasPressed(B::Up)) volUpPressedAt_ = millis();
    if (mappedInput.wasPressed(B::Down)) volDownPressedAt_ = millis();
    if (mappedInput.wasReleased(B::Up)) {
      const bool longPress = volUpPressedAt_ != 0 && (millis() - volUpPressedAt_) >= REMOTE_BTN_LONG_MS;
      volUpPressedAt_ = 0;
      remote_->sendButton(longPress ? "prev" : "playpause");
      return;
    }
    if (mappedInput.wasReleased(B::Down)) {
      const bool longPress = volDownPressedAt_ != 0 && (millis() - volDownPressedAt_) >= REMOTE_BTN_LONG_MS;
      volDownPressedAt_ = 0;
      if (longPress) {
        toggleRemoteSession();  // long-press Volume Down ends the session
      } else {
        remote_->sendButton("next");
      }
      return;
    }
    // Pump the WebSocket every loop FIRST (so it keeps running even while a volume
    // button is held), then swallow the rocker's hold so detectPageTurn() never
    // pages on it.
    remote_->update();
    remoteReportPositionIfChanged();  // emit `pos` when the user turns pages on the X4
    if (mappedInput.isPressed(B::Up) || mappedInput.isPressed(B::Down)) {
      return;
    }
  }
#elif defined(PHASE1_HIGHLIGHT_DEBUG)
  // Phase 1 debug trigger: Volume Up cycles the highlighted sentence on the
  // current page. Fire on the PRESS edge and return early so this preempts
  // detectPageTurn() (which also maps to Volume Up) — in this debug build Volume
  // Up is highlight-only; page with the Left/Right bottom-edge buttons.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    hlCycleNext();
    return;
  }
#endif

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // Enter reader menu activity on short-press Confirm. A long-press that fired a bound
  // function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      const int currentPage = section ? section->currentPage + 1 : 0;
      const int totalPages = section ? section->pageCount : 0;
      float bookProgress = 0.0f;
      if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                                 renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                                 SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty()),
                             [this](const ActivityResult& result) {
                               // Always apply orientation change even if the menu was cancelled
                               const auto& menu = std::get<MenuResult>(result.data);
                               applyOrientation(menu.orientation);
                               toggleAutoPageTurn(menu.pageTurnOption);
                               if (!result.isCancelled) {
                                 onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                               }
                             });
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        // Hold ~1s launches KOReader sync. If sync can't run (no credentials stored), fall
        // through so the normal Confirm-release still opens the reader menu.
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          if (launchKOReaderSync()) {
            ignoreNextConfirmRelease = true;  // sync launched or error shown; suppress menu open
            return;
          }
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

#ifdef PHASE2_REMOTE_DEBUG
  // The user is navigating locally: lock out inbound phone nav immediately (so an
  // in-flight command can't fight it) and flag a `pos` to emit once it renders.
  if (remote_ && remote_->isActive()) {
    remoteSuppressUntil_ = millis() + 1000;
    remotePendingPosEmit_ = true;
  }
#endif

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
        RenderLock lock(*this);
        currentSpineIndex = sync.spineIndex;
        nextPageNumber = sync.page;
        section.reset();
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;  // acted: launched the sync activity
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
      LOG_DBG("ERS", "Cache not found, building...");

      GUI.drawPopup(renderer, tr(STR_INDEXING));

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, popupFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        section.reset();
        showPendingSyncSaveError();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

#ifdef PHASE2_REMOTE_DEBUG
  // Phase 3 page-follow: now that the section is loaded, resolve a pending
  // paragraph jump (<p> ordinal) to its page. Runs whether or not the section was
  // just reloaded, so same-spine jumps work too.
  if (pendingParagraphJump.has_value() && section) {
    const uint16_t targetPara = *pendingParagraphJump;
    pendingParagraphJump.reset();
    if (const auto est = section->getPageForParagraphIndex(targetPara)) {
      int resolved = *est;
      // The LUT lands ~1 page early; peek forward (geometry only, NO display) to the
      // page that actually contains the paragraph, so we render the right page in a
      // single pass — no intermediate "wrong page" flashes.
      const int maxPage = section->pageCount - 1;
      for (int c = *est; c <= std::min(*est + 4, maxPage); ++c) {
        section->currentPage = c;
        auto pg = section->loadPageFromSectionFile();
        bool found = false;
        if (pg) {
          for (const auto& el : pg->elements) {
            if (el->getTag() == TAG_PageLine &&
                static_cast<PageLine*>(el.get())->getBlock()->getParagraphIndex() == targetPara) {
              found = true;
              break;
            }
          }
        }
        if (found) {
          resolved = c;
          break;
        }
      }
      section->currentPage = resolved;
      LOG_DBG("ERS", "Remote: paragraph %u -> page %d", targetPara, resolved);
    }
  }
#endif

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                     SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
#ifdef PHASE1_HIGHLIGHT_DEBUG
namespace {
// True if the word is empty or only whitespace / em-space sentinels (U+2003).
bool hlIsBlankWord(const std::string& w) {
  for (size_t i = 0; i < w.size();) {
    if (static_cast<unsigned char>(w[i]) == 0xE2 && i + 2 < w.size() &&
        static_cast<unsigned char>(w[i + 1]) == 0x80 && static_cast<unsigned char>(w[i + 2]) == 0x83) {
      i += 3;  // em-space sentinel
      continue;
    }
    if (w[i] != ' ' && w[i] != '\t') return false;
    i++;
  }
  return true;
}

// True if the word ends a sentence: terminal . ! ? or … (U+2026), after peeling
// trailing closing quotes/brackets (ASCII and the common UTF-8 curly quotes).
bool hlEndsSentence(const std::string& w) {
  size_t end = w.size();
  auto endsWithUtf8 = [&](const char* seq, size_t n) { return end >= n && memcmp(w.data() + end - n, seq, n) == 0; };
  if (endsWithUtf8("\xE2\x80\xA6", 3)) return true;  // …
  bool peeled = true;
  while (peeled && end > 0) {
    peeled = false;
    const char c = w[end - 1];
    if (c == '"' || c == '\'' || c == ')' || c == ']') {
      end -= 1;
      peeled = true;
      continue;
    }
    if (endsWithUtf8("\xE2\x80\x9D", 3) || endsWithUtf8("\xE2\x80\x99", 3)) {  // ” ’
      end -= 3;
      peeled = true;
    }
  }
  if (end == 0) return false;
  const char c = w[end - 1];
  return c == '.' || c == '!' || c == '?';
}
}  // namespace

void EpubReaderActivity::hlEnsurePageCache() {
  if (!section) return;
  if (hlPageIdx == section->currentPage && !hlLines.empty()) return;  // cache still valid

  hlLines.clear();
  hlSentences.clear();
  hlCurrent = -1;
  hlSavedValid = false;  // page changed -> any clean snapshot is stale

  int t, r, b, l;
  renderer.getOrientedViewableTRBL(&t, &r, &b, &l);
  hlMarginTop = t + SETTINGS.screenMargin;
  hlMarginLeft = l + SETTINGS.screenMargin;
  hlFontId = SETTINGS.getReaderFontId();

  auto page = section->loadPageFromSectionFile();
  if (!page) return;
  for (const auto& el : page->elements) {
    if (el->getTag() != TAG_PageLine) continue;
    auto* line = static_cast<PageLine*>(el.get());
    hlLines.push_back({line->getBlock(), el->xPos, el->yPos});
  }
  hlPageIdx = section->currentPage;
  hlScanSentences();
}

void EpubReaderActivity::hlScanSentences() {
  hlSentences.clear();
  bool inSentence = false;
  SentenceSpan cur{};
  for (int li = 0; li < static_cast<int>(hlLines.size()); ++li) {
    if (!hlLines[li].block) continue;
    const auto& words = hlLines[li].block->getWords();
    for (int wi = 0; wi < static_cast<int>(words.size()); ++wi) {
      const std::string& w = words[wi];
      if (!inSentence) {
        if (hlIsBlankWord(w)) continue;  // don't start a sentence on whitespace
        cur.firstLineIdx = li;
        cur.firstWordIdx = wi;
        inSentence = true;
      }
      cur.lastLineIdx = li;
      cur.lastWordIdx = wi;
      if (hlEndsSentence(w)) {
        hlSentences.push_back(cur);
        inSentence = false;
      }
    }
  }
  if (inSentence) hlSentences.push_back(cur);  // trailing partial sentence on the page
}

void EpubReaderActivity::hlSnapshotCleanPage() {
  if (hlSavedValid) return;
  if (!hlSavedBuffer) {
    hlSavedBufferSize = renderer.getBufferSize();
    hlSavedBuffer = makeUniqueNoThrow<uint8_t[]>(hlSavedBufferSize);
    if (!hlSavedBuffer) {
      LOG_ERR("HL", "snapshot malloc failed: %u bytes", static_cast<unsigned>(hlSavedBufferSize));
      hlSavedBufferSize = 0;
      return;
    }
  }
  memcpy(hlSavedBuffer.get(), renderer.getFrameBuffer(), hlSavedBufferSize);
  hlSavedValid = true;
}

void EpubReaderActivity::hlDrawSentence(const SentenceSpan& span) {
  const int lineH = renderer.getLineHeight(hlFontId);
  for (int li = span.firstLineIdx; li <= span.lastLineIdx; ++li) {
    if (li < 0 || li >= static_cast<int>(hlLines.size()) || !hlLines[li].block) continue;
    const auto& HL = hlLines[li];
    const auto& words = HL.block->getWords();
    const auto& xpos = HL.block->getWordXpos();
    const auto& styles = HL.block->getWordStyles();
    const int wordCount = static_cast<int>(words.size());
    int w0 = (li == span.firstLineIdx) ? span.firstWordIdx : 0;
    int w1 = (li == span.lastLineIdx) ? span.lastWordIdx : wordCount - 1;
    w0 = std::max(0, w0);
    w1 = std::min(wordCount - 1, w1);
    const int lineTop = hlMarginTop + HL.yPos;

    int rectX0 = std::numeric_limits<int>::max();
    int rectX1 = std::numeric_limits<int>::min();
    for (int wi = w0; wi <= w1; ++wi) {
      if (hlIsBlankWord(words[wi])) continue;
      const auto st = (wi < static_cast<int>(styles.size())) ? styles[wi] : EpdFontFamily::REGULAR;
      const int wx = hlMarginLeft + HL.xPos + (wi < static_cast<int>(xpos.size()) ? xpos[wi] : 0);
      const int ww = renderer.getTextWidth(hlFontId, words[wi].c_str(), st);
      rectX0 = std::min(rectX0, wx);
      rectX1 = std::max(rectX1, wx + ww);
    }
    if (rectX0 > rectX1) continue;  // line had only blanks in range

    renderer.fillRect(rectX0, lineTop, rectX1 - rectX0, lineH, true);
    for (int wi = w0; wi <= w1; ++wi) {
      if (hlIsBlankWord(words[wi])) continue;
      const auto st = (wi < static_cast<int>(styles.size())) ? styles[wi] : EpdFontFamily::REGULAR;
      const int wx = hlMarginLeft + HL.xPos + (wi < static_cast<int>(xpos.size()) ? xpos[wi] : 0);
      renderer.drawText(hlFontId, wx, lineTop, words[wi].c_str(), /*black=*/false, st);
    }
  }
}

void EpubReaderActivity::hlRefresh(int ordinal) {
  hlSnapshotCleanPage();
  if (!hlSavedValid) return;
  memcpy(renderer.getFrameBuffer(), hlSavedBuffer.get(), hlSavedBufferSize);  // clean slate
  if (ordinal >= 0 && ordinal < static_cast<int>(hlSentences.size())) {
    hlDrawSentence(hlSentences[ordinal]);
  }
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);  // full-frame refresh; HALF = clean (same as page turns)
}

void EpubReaderActivity::hlCycleNext() {
  RenderLock lock(*this);  // SD read (page cache) + framebuffer draw — serialize vs render task
  hlEnsurePageCache();
  if (hlSentences.empty()) return;
  hlCurrent = (hlCurrent + 1) % static_cast<int>(hlSentences.size());
  LOG_INF("HL", "highlight sentence %d/%d", hlCurrent + 1, static_cast<int>(hlSentences.size()));
  hlRefresh(hlCurrent);
}
#endif  // PHASE1_HIGHLIGHT_DEBUG

#ifdef PHASE2_REMOTE_DEBUG
int EpubReaderActivity::remoteSentenceCount() {
  RenderLock lock(*this);  // hlEnsurePageCache reads the SD — serialize vs render task
  hlEnsurePageCache();
  return static_cast<int>(hlSentences.size());
}

bool EpubReaderActivity::remoteHighlightSentence(int ordinal) {
  // Re-render the clean page first (no lock held), then read geometry + draw under
  // a RenderLock so this main-task path doesn't race the render task.
  requestUpdateAndWait();
  RenderLock lock(*this);
  hlEnsurePageCache();
  if (hlSentences.empty()) return false;
  if (ordinal < 0) ordinal = 0;
  if (ordinal >= static_cast<int>(hlSentences.size())) ordinal = static_cast<int>(hlSentences.size()) - 1;
  hlCurrent = ordinal;
  hlRefresh(ordinal);  // snapshots clean page (lazy), draws highlight, HALF refresh
  return true;
}

bool EpubReaderActivity::remoteGotoParagraph(int spine, int para) {
  if (!epub) return false;
  if (millis() < remoteSuppressUntil_) return false;  // user is in control; ignore phone nav
  if (para < 0) para = 0;
  const int spineCount = epub->getSpineItemsCount();
  {
    RenderLock lock(*this);
    if (spine >= 0 && spine < spineCount && spine != currentSpineIndex) {
      currentSpineIndex = spine;
      nextPageNumber = 0;
      section.reset();  // force reload of the target spine
    }
    pendingParagraphJump = static_cast<uint16_t>(para);
  }
  requestUpdateAndWait();  // render resolves paragraph -> correct page (peek) and paints, once
  // Phone-driven navigation: update the baseline so it isn't echoed back as a `pos`.
  lastReportedSpine_ = currentSpineIndex;
  lastReportedPage_ = section ? section->currentPage : -1;
  return true;
}

int EpubReaderActivity::remoteCurrentParagraph() {
  return currentTopParagraph_;  // cached by the render task; never touches the SD here
}

std::string EpubReaderActivity::remoteFilePath() const { return epub ? epub->getPath() : std::string(); }

std::string EpubReaderActivity::remotePosDiag() {
  char buf[220];
  snprintf(buf, sizeof(buf),
           "active=%d spine=%d curPage=%d rndSpine=%d rndPage=%d topPara=%d lastSpine=%d lastPage=%d suppress=%ld",
           (remote_ && remote_->isActive()) ? 1 : 0, currentSpineIndex, section ? section->currentPage : -1,
           currentRenderedSpine_, currentRenderedPage_, currentTopParagraph_, lastReportedSpine_, lastReportedPage_,
           static_cast<long>(remoteSuppressUntil_ > millis() ? remoteSuppressUntil_ - millis() : 0));
  return std::string(buf);
}

void EpubReaderActivity::remoteReportPositionIfChanged() {
  if (!remote_ || !remote_->isActive() || !section) return;
  if (!remotePendingPosEmit_) return;  // only emit for user-initiated navigation
  // Wait until the render has settled on the new page so the paragraph is correct.
  // (The suppress window set at the button press keeps phone nav from moving it.)
  if (currentRenderedSpine_ != currentSpineIndex || currentRenderedPage_ != section->currentPage) return;
  remotePendingPosEmit_ = false;
  lastReportedSpine_ = currentRenderedSpine_;
  lastReportedPage_ = currentRenderedPage_;
  remote_->sendPos(currentRenderedSpine_, currentTopParagraph_);  // plain-int reads; no SD here
}


bool EpubReaderActivity::remoteHighlightParaSentence(int spine, int para, int sent) {
  if (millis() < remoteSuppressUntil_) return false;  // user is in control; ignore phone nav
  if (para < 1) para = 1;
  // 1) Navigate so paragraph `para` is on screen (renders the clean page). This
  //    runs requestUpdateAndWait() with NO lock held.
  remoteGotoParagraph(spine, para);

  // Serialize the SD read + framebuffer draw against the render task. Every other
  // draw/SD site in this file holds a RenderLock; the remote paths run on the main
  // (loop/WebSocket) task and must too, or they race the render task (torn frame /
  // panel hang / corrupt SD read). remoteGotoParagraph already released its lock.
  RenderLock lock(*this);
  // 2) Build line geometry for the now-current page.
  hlEnsurePageCache();
  if (hlLines.empty()) return false;

  // 3) Segment this paragraph's sentences from its lines on this page (same rule
  //    as the device's scanner). Spans index into hlLines.
  std::vector<SentenceSpan> paraSentences;
  bool inSentence = false;
  SentenceSpan cur{};
  for (int li = 0; li < static_cast<int>(hlLines.size()); ++li) {
    if (!hlLines[li].block || hlLines[li].block->getParagraphIndex() != static_cast<uint16_t>(para)) continue;
    const auto& words = hlLines[li].block->getWords();
    for (int wi = 0; wi < static_cast<int>(words.size()); ++wi) {
      if (!inSentence) {
        if (hlIsBlankWord(words[wi])) continue;
        cur.firstLineIdx = li;
        cur.firstWordIdx = wi;
        inSentence = true;
      }
      cur.lastLineIdx = li;
      cur.lastWordIdx = wi;
      if (hlEndsSentence(words[wi])) {
        paraSentences.push_back(cur);
        inSentence = false;
      }
    }
  }
  if (inSentence) paraSentences.push_back(cur);
  if (paraSentences.empty()) return false;
  if (sent < 0) sent = 0;
  if (sent >= static_cast<int>(paraSentences.size())) sent = static_cast<int>(paraSentences.size()) - 1;

  // 4) Draw the highlight directly onto the freshly-rendered clean page. The
  //    navigate in step 1 already painted the clean page into the framebuffer, so
  //    no 48KB snapshot is needed — that second framebuffer was the heap pressure
  //    that OOM'd (terminate/abort) under sustained load with Wi-Fi up.
  hlDrawSentence(paraSentences[sent]);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  hlCurrent = -1;  // page-relative cycle state no longer applies
  LOG_INF("HL", "remote highlight spine=%d para=%d sent=%d/%d", spine, para, sent + 1,
          static_cast<int>(paraSentences.size()));
  return true;
}

std::string EpubReaderActivity::remoteDiag(int para) {
  if (para < 1) para = 1;
  const int pageForPara =
      section ? static_cast<int>(section->getPageForParagraphIndex(static_cast<uint16_t>(para)).value_or(0xFFFF)) : -1;
  remoteGotoParagraph(-1, para);  // land where the lookup says paragraph `para` is
  RenderLock lock(*this);         // hlEnsurePageCache reads the SD — serialize vs render task
  hlEnsurePageCache();
  // Distinct paragraph indices present on the landed page, in order.
  std::string paras;
  int last = -1;
  for (const auto& l : hlLines) {
    if (!l.block) continue;
    const int p = l.block->getParagraphIndex();
    if (p != last) {
      paras += std::to_string(p);
      paras += ",";
      last = p;
    }
  }
  char buf[200];
  snprintf(buf, sizeof(buf), "spine=%d page=%d/%d lookup(para %d)->page %d  parasOnPage=[%s]", currentSpineIndex,
           section ? section->currentPage : -1, section ? section->pageCount : -1, para, pageForPara, paras.c_str());
  return std::string(buf);
}

bool EpubReaderActivity::remoteHighlightParagraph(int spine, int para) {
  if (millis() < remoteSuppressUntil_) return false;  // user is in control
  if (para < 1) para = 1;
  // Navigate so the paragraph is on screen (renders the clean page into the framebuffer).
  remoteGotoParagraph(spine, para);
  RenderLock lock(*this);  // serialize SD read + framebuffer draw vs the render task
  hlEnsurePageCache();
  if (hlLines.empty()) return false;

  // Vertical extent of this paragraph's lines on the current page.
  const int lineH = renderer.getLineHeight(hlFontId);
  int top = std::numeric_limits<int>::max();
  int bottom = std::numeric_limits<int>::min();
  for (const auto& l : hlLines) {
    if (!l.block || l.block->getParagraphIndex() != static_cast<uint16_t>(para)) continue;
    const int lt = hlMarginTop + l.yPos;
    top = std::min(top, lt);
    bottom = std::max(bottom, lt + lineH);
  }
  if (top > bottom) return false;  // paragraph not on the resolved page

  // Left-margin accent bar spanning the paragraph (no text inversion). Fills the
  // left margin so it's clearly visible without touching the text. Draws directly
  // on the already-clean framebuffer.
  const int barX = 0;
  const int barW = std::max(6, hlMarginLeft - 1);  // up to the text edge
  renderer.fillRect(barX, top, barW, bottom - top, true);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  hlCurrent = -1;
  LOG_INF("HL", "remote highlight paragraph spine=%d para=%d", spine, para);
  return true;
}

void EpubReaderActivity::drawRemoteStatus(const char* line1, const char* line2) {
  RenderLock lock(*this);  // main-task draw — serialize vs the render task
  renderer.clearScreen();
  const int cy = renderer.getScreenHeight() / 2;
  renderer.drawCenteredText(NOTOSANS_16_FONT_ID, cy - 24, line1);
  if (line2 && line2[0]) renderer.drawCenteredText(NOTOSANS_14_FONT_ID, cy + 8, line2);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void EpubReaderActivity::drawWifiGlyph(int cx, int cyDot) const {
  // Solid dot, then three arcs fanning upward over it (classic Wi-Fi symbol).
  renderer.fillRoundedRect(cx - 2, cyDot - 2, 5, 5, 2, Color::Black);
  const int radii[3] = {5, 9, 13};
  for (int a = 0; a < 3; ++a) {
    const int rad = radii[a];
    int px = -1, py = -1;
    for (int deg = 218; deg <= 322; deg += 13) {  // ~from upper-left to upper-right over the top
      const double t = deg * 3.14159265358979 / 180.0;
      const int x = cx + static_cast<int>(std::lround(rad * std::cos(t)));
      const int y = cyDot + static_cast<int>(std::lround(rad * std::sin(t)));
      if (px >= 0) renderer.drawLine(px, py, x, y, 3, /*black=*/true);
      px = x;
      py = y;
    }
  }
}

void EpubReaderActivity::drawRemoteIndicatorIfActive() const {
#ifdef PHASE2_REMOTE_DEBUG
  if (!remote_ || !remote_->isActive()) return;
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const int barH = UITheme::getInstance().getStatusBarHeight();
  if (barH > 0) {
    // In the bottom status bar, just right of the far-left battery indicator.
    const int cx = 80;           // estimate: clears the "100%" + battery icon on the left
    const int cyDot = sh - 6;    // dot near the bar's baseline; arcs fan up into the bar
    renderer.fillRoundedRect(cx - 15, sh - barH + 2, 30, barH - 3, 4, Color::White);
    drawWifiGlyph(cx, cyDot);
  } else {
    // No status bar shown: fall back to the top-right corner.
    const int cx = sw - 20, cyDot = 21;
    renderer.fillRoundedRect(cx - 17, 3, 34, 24, 5, Color::White);
    drawWifiGlyph(cx, cyDot);
  }
#endif
}

void EpubReaderActivity::drawRemoteResult(bool ok, const char* title, const char* subtitle) {
  RenderLock lock(*this);  // main-task draw — serialize vs the render task
  renderer.clearScreen();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const int cx = w / 2;
  const int badgeCy = h / 2 - 78;
  const int r = 46;

  // Filled black disc as the badge.
  renderer.fillRoundedRect(cx - r, badgeCy - r, 2 * r, 2 * r, r, Color::Black);

  // White glyph inside: checkmark on success, X on failure.
  const int lw = 8;
  if (ok) {
    renderer.drawLine(cx - 21, badgeCy + 3, cx - 6, badgeCy + 18, lw, /*state(white)=*/false);
    renderer.drawLine(cx - 6, badgeCy + 18, cx + 23, badgeCy - 18, lw, false);
  } else {
    renderer.drawLine(cx - 18, badgeCy - 18, cx + 18, badgeCy + 18, lw, false);
    renderer.drawLine(cx - 18, badgeCy + 18, cx + 18, badgeCy - 18, lw, false);
  }

  // Title (bold) + lighter subtitle below the badge.
  int ty = badgeCy + r + 30;
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, ty, title, true, EpdFontFamily::BOLD);
  if (subtitle && subtitle[0]) {
    ty += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 12;
    renderer.drawCenteredText(NOTOSANS_14_FONT_ID, ty, subtitle, true);
  }
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void EpubReaderActivity::toggleRemoteSession() {
  if (remote_ && remote_->isActive()) {
    remote_->stop();
    remote_.reset();
    requestUpdate(true);  // redraw the page
    return;
  }
  drawRemoteStatus("Remote session", "Connecting Wi-Fi...");
  remote_ = makeUniqueNoThrow<RemoteReaderController>(*this);  // bare new aborts on OOM (-fno-exceptions)
  if (!remote_) {
    drawRemoteResult(false, "Couldn't connect", "Out of memory");
    remoteAwaitDismiss_ = true;
    return;
  }
  if (remote_->begin()) {
    const std::string l2 = "crosspoint.local  ·  " + remote_->ip();
    drawRemoteResult(true, "Connected", l2.c_str());
    // Baseline the position so the resume-to-book render below isn't echoed as `pos`.
    lastReportedSpine_ = currentSpineIndex;
    lastReportedPage_ = section ? section->currentPage : -1;
    delay(1600);            // brief glimpse of the address (mDNS means it's rarely needed)
    requestUpdate(true);    // return to the book; the session stays live in the background
  } else {
    // Hold the failure on screen until the user acknowledges with any button.
    drawRemoteResult(false, "Couldn't connect", remote_->status().c_str());
    remote_.reset();
    remoteAwaitDismiss_ = true;
  }
}
#endif  // PHASE2_REMOTE_DEBUG

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();

  // Capture top-of-page paragraph for remote position sync, here in the render
  // task where the page is already loaded (the main task must NOT touch the SD).
  currentRenderedSpine_ = currentSpineIndex;
  currentRenderedPage_ = section ? section->currentPage : -1;
  currentTopParagraph_ = 0;
  for (const auto& el : page->elements) {
    if (el->getTag() == TAG_PageLine) {
      currentTopParagraph_ = static_cast<PageLine*>(el.get())->getBlock()->getParagraphIndex();
      break;
    }
  }
  const int fontId = SETTINGS.getReaderFontId();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        const auto tEnd = millis();
        LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked);
#ifdef PHASE2_REMOTE_DEBUG
  drawRemoteIndicatorIfActive();  // top-right Wi-Fi glyph while a remote session is live
#endif
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  const std::string bmPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->pageCount;
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  const bool ok = JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str());
  if (!ok) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const ProgressRange pageRange =
      getPageProgressRange(epub, currentSpineIndex, section->currentPage, section->pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, section->pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
