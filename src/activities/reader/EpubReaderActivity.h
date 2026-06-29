#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#include "BookmarkEntry.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#ifdef PHASE2_REMOTE_DEBUG
#include <memory>

#include "RemoteReaderController.h"
#endif

class TextBlock;  // for Phase 1 highlight geometry (held by shared_ptr)

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;

  // Captured by the render task each render (no extra I/O), read by the main task
  // for remote position sync — avoids SD access races with the render task.
  int currentTopParagraph_ = 0;
  int currentRenderedSpine_ = -1;
  int currentRenderedPage_ = -1;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

#ifdef PHASE1_HIGHLIGHT_DEBUG
  // --- Phase 1: sentence-highlight primitive (debug-only, no networking) ---
  // A sentence as a half-open-ish word range over the current page's laid-out
  // lines (lastWordIdx inclusive). Never spans pages in Phase 1.
  struct SentenceSpan {
    int firstLineIdx;
    int firstWordIdx;
    int lastLineIdx;
    int lastWordIdx;
  };
  // Lightweight retained geometry for one text line on the current page. The
  // TextBlock is held by shared_ptr so it survives after the source Page is freed.
  struct HighlightLine {
    std::shared_ptr<TextBlock> block;
    int16_t xPos;
    int16_t yPos;
  };
  std::vector<HighlightLine> hlLines;       // current page's text lines, in order
  std::vector<SentenceSpan> hlSentences;    // scanned sentences for current page
  int hlPageIdx = -1;                       // section->currentPage the caches were built for
  int hlCurrent = -1;                       // highlighted sentence ordinal (-1 = none)
  int hlFontId = 0;
  int hlMarginTop = 0;
  int hlMarginLeft = 0;
  std::unique_ptr<uint8_t[]> hlSavedBuffer; // clean-page framebuffer snapshot (lazy)
  size_t hlSavedBufferSize = 0;
  bool hlSavedValid = false;

  void hlEnsurePageCache();                 // (re)load page geometry + scan sentences if stale
  void hlScanSentences();                   // build hlSentences from hlLines (punctuation-based)
  void hlSnapshotCleanPage();               // capture the clean framebuffer once per page
  void hlDrawSentence(const SentenceSpan& span);  // fill rect + inverted text behind the sentence
  void hlRefresh(int ordinal);              // refreshAfterHighlight: re-blit + draw + one FAST refresh
  void hlCycleNext();                       // debug trigger: advance to the next sentence
#endif

#ifdef PHASE2_REMOTE_DEBUG
  // --- Phase 2: remote session (Wi-Fi + WebSocket inside the reader) ---
  std::unique_ptr<RemoteReaderController> remote_;
  // Phase 3 page-follow: a paragraph (<p> ordinal) to navigate to, resolved to a
  // page once the section is loaded (works same-spine and cross-spine).
  std::optional<uint16_t> pendingParagraphJump;
  void toggleRemoteSession();               // debug trigger: start/stop the remote session
  void drawRemoteStatus(const char* line1, const char* line2);  // plain centered status (e.g. "Connecting…")
  void drawRemoteResult(bool ok, const char* title, const char* subtitle);  // badge + title + subtitle
  void drawWifiGlyph(int cx, int cyDot) const;  // small Wi-Fi fan (dot + 3 arcs) at (cx, dot)
  void drawRemoteIndicatorIfActive() const;     // top-right session indicator, drawn each render
  // Emit a {"evt":"pos",...} when the user navigates ON the X4 (page/spine changed
  // since last report). Phone-driven navigation updates the baseline so it doesn't echo.
  void remoteReportPositionIfChanged();
  int lastReportedSpine_ = -1;
  int lastReportedPage_ = -1;
  // After local (user) navigation, ignore inbound goto/highlight until this time, so
  // an in-flight phone command can't snap the page back before the phone hears `pos`.
  unsigned long remoteSuppressUntil_ = 0;
  // While true, a failure/status message is held on screen until any button dismisses it.
  bool remoteAwaitDismiss_ = false;
  // Set when the USER turns a page (button), cleared once the new page renders and
  // its `pos` is emitted. Distinguishes user navigation from phone-driven nav.
  bool remotePendingPosEmit_ = false;
#endif

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
#ifdef PHASE2_REMOTE_DEBUG
  // Keep the reader awake while a remote session holds Wi-Fi up.
  bool preventAutoSleep() override { return remote_ && remote_->isActive(); }
  // Driven by RemoteReaderController over the WebSocket:
  // highlight sentence `ordinal` (0-based) on the current page; returns false if
  // the page has no sentences. Re-renders the clean page first if needed.
  bool remoteHighlightSentence(int ordinal);
  // Number of detected sentences on the current page (ensures the cache first).
  int remoteSentenceCount();
  // Page-follow: navigate so the page containing paragraph `para` (<p> ordinal,
  // 1-based) of `spine` is shown. spine < 0 means "current spine". No highlight.
  bool remoteGotoParagraph(int spine, int para);
  // Precise highlight: navigate to paragraph `para` (<p> ordinal, 1-based) of
  // `spine` (-1 = current), then highlight sentence `sent` (0-based) within that
  // paragraph using the shared punctuation rule. Returns false if not found on the
  // landed page. (v1: a paragraph spanning pages only resolves sentences on its
  // start page.)
  bool remoteHighlightParaSentence(int spine, int para, int sent);
  // Paragraph-granularity highlight: navigate to paragraph `para` and mark the whole
  // paragraph with a calm left-margin accent bar (far fewer refreshes than per-sentence).
  bool remoteHighlightParagraph(int spine, int para);
  // Debug: after landing on paragraph `para`'s page, report the page + the distinct
  // paragraph indices actually present on it (to diagnose page-lookup alignment).
  std::string remoteDiag(int para);
  // Current reading position for position-sync: spine index, top-of-page paragraph
  // (<p> ordinal, from correct line stamps), and the book's file path.
  int remoteCurrentSpine() const { return currentSpineIndex; }
  int remoteCurrentParagraph();
  std::string remoteFilePath() const;
  std::string remotePosDiag();  // debug: raw position-sync state (no nav, no SD)
#endif
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
