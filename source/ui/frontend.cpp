#include "frontend.h"

#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>

#include <3ds.h>

#include "../utils/debug.h"
#include "canvas.h"
#include "logbuffer.h"
#include "screens.h"
#include "touchinput.h"

using namespace ui;

static_assert(K_A == KEY_A && K_B == KEY_B && K_X == KEY_X && K_Y == KEY_Y &&
                  K_L == KEY_L && K_R == KEY_R && K_START == KEY_START &&
                  K_SELECT == KEY_SELECT && K_DUP == KEY_DUP && K_DDOWN == KEY_DDOWN &&
                  K_DLEFT == KEY_DLEFT && K_DRIGHT == KEY_DRIGHT,
              "ui::Key must match libctru's KEY_* bits");
static_assert(K_LIST_ROW0 > KEY_ZR && (K_LIST_ROWS & KEY_TOUCH) == 0,
              "the conflict list's row bits must be ones libctru leaves unused");

namespace frontend
{

// ---------------------------------------------------------------------------
// State shared between the UI thread and the worker
// ---------------------------------------------------------------------------
// g_logLock guards the log; g_stateLock guards g_shared and the prompt fields.
// Neither is held while calling out, and the UI thread never prints while it
// holds one, so the two cannot deadlock against the stdout hook.
// ---------------------------------------------------------------------------
static LightLock g_logLock;
static LogBuffer g_log(LOG_COLUMNS, 3000);
static LogScroll g_scroll(LOG_ROWS);

static LightLock g_stateLock;
static BottomModel g_shared;
static int g_answer;            // prompt answer, -1 while unanswered
static LightEvent g_answered;   // signalled when g_answer is set
static bool g_cancel;           // START during a run
static bool g_closing;          // the system asked the app to close
static volatile bool g_workerDone;
static u64 g_runStarted;

static Canvas *g_top;
static Canvas *g_bottom;
static TouchInput g_touch;

// ---------------------------------------------------------------------------
// stdout
// ---------------------------------------------------------------------------
static ssize_t logWrite(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    (void)r;
    (void)fd;
    LightLock_Lock(&g_logLock);
    g_log.append(ptr, len);
    debugLogAppend(ptr, len);
    LightLock_Unlock(&g_logLock);
    return (ssize_t)len;
}

static devoptab_t g_logDevoptab;

static void installStdout()
{
    memset(&g_logDevoptab, 0, sizeof(g_logDevoptab));
    g_logDevoptab.name = "con";
    g_logDevoptab.write_r = logWrite;
    devoptab_list[STD_OUT] = &g_logDevoptab;
    devoptab_list[STD_ERR] = &g_logDevoptab;
    // Unbuffered, as libctru's console is: a line printed just before a long
    // network call should be on screen during it, not after.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
// The canvases are landscape and row-major; the framebuffers are the panels'
// native portrait orientation, one column of the picture per 240-pixel run,
// bottom row first.  Copied in 16-row bands so both sides stay in cache.
static void present(const Canvas &c, gfxScreen_t screen)
{
    u16 fbW = 0, fbH = 0;
    u16 *fb = (u16 *)gfxGetFramebuffer(screen, GFX_LEFT, &fbW, &fbH);
    const int W = c.width(), H = c.height();
    const Color *px = c.pixels();
    for (int band = 0; band < H; band += 16)
        for (int x = 0; x < W; x++)
        {
            u16 *col = fb + x * H + (H - 1 - band);
            const Color *src = px + band * W + x;
            for (int y = band; y < band + 16 && y < H; y++, src += W)
                *col-- = *src;
        }
    GSPGPU_FlushDataCache(fb, W * H * sizeof(u16));
    gfxScreenSwapBuffers(screen, false);
}

static uint32_t g_frame;
static uint32_t g_drawnRevision = ~0u;
static size_t g_drawnTop = ~(size_t)0;
static bool g_drawnFollow, g_drawnBusy, g_drawnDebug;
static std::string g_drawnBottom;

// Everything the bottom screen shows, as one comparable string: redrawing
// costs a few milliseconds, comparing this costs nothing.
static std::string signature(const BottomModel &m)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%d|%d|%d|%d|%d|%u|%u|%d|%d|%u|%d|%lx|%u|%u|%u|%d|%d|%d|%d|%d|%u",
             m.mode,
             m.debugRun, m.starting, m.configLoaded, m.pathCount, m.elapsedSeconds, m.spinnerFrame,
             m.cancelling, m.applyToAll, m.logKilobytes, m.logTrimmed,
             (unsigned long)m.heldKeys, (unsigned)m.logFirst, (unsigned)m.logEnd,
             (unsigned)m.logTop, m.lastResultOk, m.logVisible, m.conflictPick,
             m.configIssueCount, m.configScroll,
             // The problem lines are set once at startup and never change
             // while shown, so their count stands in for them.
             (unsigned)m.configIssueLines.size());
    std::string sides;
    for (auto &side : m.conflictSides)
        sides += side + "|";
    return std::string(buf) + m.remotes + "|" + m.lastResult + "|" + m.provider + "|" +
           m.entry + "|" + m.conflictPath + "|" + m.fileFilter + "|" + sides;
}

static void redraw(const BottomModel &m, const std::vector<Button> &buttons)
{
    LightLock_Lock(&g_logLock);
    TopModel tm;
    tm.debugRun = m.debugRun;
    tm.busy = m.mode != SCREEN_MENU;
    bool topDirty = g_log.revision() != g_drawnRevision || g_scroll.top() != g_drawnTop ||
                    g_scroll.following() != g_drawnFollow || tm.busy != g_drawnBusy ||
                    tm.debugRun != g_drawnDebug;
    if (topDirty)
    {
        drawTop(*g_top, tm, g_log, g_scroll);
        g_drawnRevision = g_log.revision();
        g_drawnTop = g_scroll.top();
        g_drawnFollow = g_scroll.following();
        g_drawnBusy = tm.busy;
        g_drawnDebug = tm.debugRun;
    }
    LightLock_Unlock(&g_logLock);
    if (topDirty)
        present(*g_top, GFX_TOP);

    std::string sig = signature(m);
    if (sig != g_drawnBottom)
    {
        drawBottom(*g_bottom, m, buttons);
        present(*g_bottom, GFX_BOTTOM);
        g_drawnBottom = sig;
    }
}

// ---------------------------------------------------------------------------
// One UI frame
// ---------------------------------------------------------------------------
// Reads the buttons and the touch screen, scrolls the log, redraws what
// changed and waits for the next VBlank.  Returns the keys pressed this frame
// — physical or touched, they are the same bits — minus the scroll keys,
// which are handled here in every mode.  False when the app must close.
static const u32 SCROLL_KEYS = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT;
static float g_circleCarry;
static ScreenMode g_lastMode = SCREEN_MENU;

static bool frame(u32 &pressed)
{
    pressed = 0;
    if (!aptMainLoop())
        return false;
    g_frame++;

    hidScanInput();
    u32 down = hidKeysDown(), held = hidKeysHeld(), repeat = hidKeysDownRepeat();
    touchPosition touch;
    hidTouchRead(&touch);

    LightLock_Lock(&g_stateLock);
    if (g_shared.mode == SCREEN_RUNNING || g_shared.mode == SCREEN_CONFLICT)
    {
        g_shared.elapsedSeconds = (unsigned)((osGetTime() - g_runStarted) / 1000);
        g_shared.spinnerFrame = g_frame / 6;
    }
    BottomModel m = g_shared;
    LightLock_Unlock(&g_stateLock);

    // The buttons are rebuilt every frame; a touch that began on a button of
    // another screen must not land on whatever took its place.
    if (m.mode != g_lastMode)
        g_touch.cancel();
    g_lastMode = m.mode;
    std::vector<Button> buttons = bottomButtons(m);
    TouchResult t = g_touch.update((held & KEY_TOUCH) != 0, touch.px, touch.py, buttons);

    // The log scrolls in every mode: D-pad Up/Down a row (repeating while
    // held), Left/Right a page, the circle pad smoothly, and the touch strip.
    // The one exception is a conflict, whose list of sides takes the D-pad's
    // Up/Down; the strip, Left/Right and the circle pad still scroll the log
    // there.
    u32 listKeys = conflictUsesList(m) ? (repeat & (KEY_DUP | KEY_DDOWN)) : 0;
    u32 scrollKeys = ((repeat & ~listKeys) | t.pressed) & SCROLL_KEYS;
    long rows = 0;
    if (scrollKeys & KEY_DUP)
        rows -= 1;
    if (scrollKeys & KEY_DDOWN)
        rows += 1;
    if (scrollKeys & KEY_DLEFT)
        rows -= LOG_ROWS - 1;
    if (scrollKeys & KEY_DRIGHT)
        rows += LOG_ROWS - 1;
    circlePosition circle;
    hidCircleRead(&circle);
    if (circle.dy > 30 || circle.dy < -30)
    {
        // Up to about 30 rows a second at full tilt; the dead zone keeps a
        // resting thumb from creeping the log.
        g_circleCarry -= circle.dy / 300.0f;
        long whole = (long)g_circleCarry;
        rows += whole;
        g_circleCarry -= whole;
    }
    else
        g_circleCarry = 0;

    LightLock_Lock(&g_logLock);
    if (m.mode == SCREEN_CONFIG_ISSUES)
    {
        // The problems at startup take every scroll control: they are what
        // the user has to read, and the log above only repeats them.  The
        // strip shows the card's position instead of the log's.
        int top = configScrollTo(m, (int)rows, t.dragging ? t.dragFraction : -1.0f);
        if (top != m.configScroll)
        {
            LightLock_Lock(&g_stateLock);
            g_shared.configScroll = top;
            LightLock_Unlock(&g_stateLock);
            m.configScroll = top;
        }
        m.logFirst = 0;
        m.logEnd = configIssueRows(m).size();
        m.logTop = top;
        m.logVisible = CONFIG_ISSUE_ROWS;
    }
    else
    {
        if (t.dragging)
            g_scroll.scrollToFraction(g_log, t.dragFraction);
        else if (rows != 0)
            g_scroll.scrollBy(g_log, rows);
    }
    g_scroll.update(g_log);
    if (m.mode != SCREEN_CONFIG_ISSUES)
    {
        m.logFirst = g_log.firstRow();
        m.logEnd = g_log.endRow();
        m.logTop = g_scroll.top();
        m.logVisible = g_scroll.visibleRows();
    }
    LightLock_Unlock(&g_logLock);

    m.heldKeys = (held & ~KEY_TOUCH) | t.held;
    redraw(m, buttons);
    gspWaitForVBlank();

    pressed = ((down | t.pressed) & ~SCROLL_KEYS) | listKeys;
    return true;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
bool init()
{
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    LightLock_Init(&g_logLock);
    LightLock_Init(&g_stateLock);
    LightEvent_Init(&g_answered, RESET_ONESHOT);
    hidSetRepeatParameters(REPEAT_DELAY, REPEAT_INTERVAL);

    g_top = new Canvas(TOP_W, TOP_H);
    g_bottom = new Canvas(BOTTOM_W, BOTTOM_H);
    installStdout();

    u32 ignored;
    return frame(ignored);
}

void exit()
{
    delete g_top;
    delete g_bottom;
    g_top = g_bottom = NULL;
    gfxExit();
}

void setMenuInfo(bool configLoaded, const std::string &remotes, int pathCount,
                 const std::string &fileFilter, int configIssueCount)
{
    LightLock_Lock(&g_stateLock);
    g_shared.configIssueCount = configIssueCount;
    g_shared.starting = false;
    g_shared.configLoaded = configLoaded;
    g_shared.remotes = remotes;
    g_shared.pathCount = pathCount;
    g_shared.fileFilter = fileFilter;
    LightLock_Unlock(&g_stateLock);
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
bool showConfigIssues(const std::vector<std::string> &lines, int count)
{
    LightLock_Lock(&g_stateLock);
    g_shared.configIssueLines = lines;
    g_shared.configIssueCount = count;
    g_shared.configScroll = 0;
    g_shared.mode = SCREEN_CONFIG_ISSUES;
    LightLock_Unlock(&g_stateLock);

    bool go = false;
    u32 keys;
    while (frame(keys))
    {
        if (keys & KEY_A)
        {
            go = true;
            break;
        }
        if (keys & KEY_START)
            break;
    }

    LightLock_Lock(&g_stateLock);
    g_shared.mode = SCREEN_MENU;
    LightLock_Unlock(&g_stateLock);
    return go;
}

MenuChoice waitForMenu()
{
    LightLock_Lock(&g_stateLock);
    g_shared.mode = SCREEN_MENU;
    LightLock_Unlock(&g_stateLock);

    u32 keys;
    while (frame(keys))
    {
        if (keys & KEY_A)
            return MENU_SYNC;
        if (keys & KEY_Y)
            return MENU_SYNC_DEBUG;
        if (keys & KEY_START)
            return MENU_EXIT;
    }
    return MENU_EXIT;
}

// ---------------------------------------------------------------------------
// Prompts
// ---------------------------------------------------------------------------
// The worker sets the mode and sleeps on g_answered; the UI thread answers by
// setting g_answer, putting the mode back to "running" and signalling.  The
// mode change is what makes a second key press in the same frame a no-op.
static int ask(ScreenMode mode)
{
    LightLock_Lock(&g_stateLock);
    if (g_closing)
    {
        LightLock_Unlock(&g_stateLock);
        return -1;
    }
    g_answer = -1;
    g_shared.mode = mode;
    LightLock_Unlock(&g_stateLock);

    LightEvent_Wait(&g_answered);

    LightLock_Lock(&g_stateLock);
    int answer = g_answer;
    LightLock_Unlock(&g_stateLock);
    return answer;
}

// A conflict's answer is the index of the side kept, or one of these.  -1 is
// what ask() returns when the app is closing, which also stops the run.
static const int ANSWER_CANCEL = -2;
static const int ANSWER_SKIP = -3;

// Call with g_stateLock held.
static void answerLocked(int answer)
{
    if (g_shared.mode != SCREEN_CONFLICT && g_shared.mode != SCREEN_SAVE_LOG)
        return;
    g_answer = answer;
    g_shared.mode = SCREEN_RUNNING;
    LightEvent_Signal(&g_answered);
}

static void handleRunKeys(u32 keys)
{
    LightLock_Lock(&g_stateLock);
    switch (g_shared.mode)
    {
    case SCREEN_RUNNING:
        if ((keys & KEY_START) && !g_cancel)
        {
            g_cancel = true;
            g_shared.cancelling = true;
        }
        break;
    case SCREEN_CONFLICT:
    {
        // A list whose highlighted side A keeps; a tapped row, R (wrapping)
        // and D-pad Up/Down move the highlight.
        int sides = (int)g_shared.conflictSides.size();
        int pick = conflictPickClamped(g_shared);
        if (keys & KEY_L)
            g_shared.applyToAll = !g_shared.applyToAll;
        else if (keys & K_LIST_ROWS)
        {
            for (int i = 0; i < CONFLICT_LIST_ROWS; i++)
                if (keys & (K_LIST_ROW0 << i))
                {
                    int side = conflictListFirst(g_shared) + i;
                    if (side < sides)
                        g_shared.conflictPick = side;
                    break;
                }
        }
        else if ((keys & KEY_R) && sides > 0)
            g_shared.conflictPick = (pick + 1) % sides;
        else if (keys & KEY_DUP)
            g_shared.conflictPick = pick > 0 ? pick - 1 : 0;
        else if (keys & KEY_DDOWN)
            g_shared.conflictPick = pick + 1 < sides ? pick + 1 : pick;
        else if (keys & KEY_A)
            answerLocked(pick);
        else if (keys & KEY_X)
            answerLocked(ANSWER_SKIP);
        else if (keys & KEY_START)
        {
            g_cancel = true;
            g_shared.cancelling = true;
            answerLocked(ANSWER_CANCEL);
        }
        break;
    }
    case SCREEN_SAVE_LOG:
        if (keys & KEY_A)
            answerLocked(1);
        else if (keys & KEY_B)
            answerLocked(0);
        break;
    default:
        break;
    }
    LightLock_Unlock(&g_stateLock);
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
struct WorkerCall
{
    void (*fn)(void *);
    void *arg;
};

static void workerEntry(void *p)
{
    WorkerCall *call = (WorkerCall *)p;
    call->fn(call->arg);
    g_workerDone = true;
}

void runWorker(void (*fn)(void *), void *arg)
{
    LightLock_Lock(&g_stateLock);
    g_shared.mode = SCREEN_RUNNING;
    g_shared.debugRun = debugEnabled();
    g_shared.provider.clear();
    g_shared.entry.clear();
    g_shared.cancelling = false;
    g_shared.applyToAll = false;
    g_cancel = false;
    g_closing = false;
    g_workerDone = false;
    g_runStarted = osGetTime();
    LightLock_Unlock(&g_stateLock);

    // The previous run stays in the scrollback; a new one starts at the end.
    LightLock_Lock(&g_logLock);
    g_scroll.scrollToEnd(g_log);
    LightLock_Unlock(&g_logLock);

    // Below the UI thread, so a frame is never late because a transfer is
    // hashing a file; on the app core, where the network calls block anyway.
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    WorkerCall call = {fn, arg};
    Thread worker = threadCreate(workerEntry, &call, 256 * 1024,
                                 prio < 0x3F ? prio + 1 : 0x3F, -2, false);
    if (worker == NULL)
    {
        // Without a thread the sync still runs, it just cannot be watched.
        debugf("threadCreate failed, running the sync on the UI thread\n");
        workerEntry(&call);
    }

    u32 keys;
    while (!g_workerDone)
    {
        if (!frame(keys))
        {
            // Closing: stop at the next file, answer any open question with
            // "stop", and wait for the worker — its manifest save included.
            LightLock_Lock(&g_stateLock);
            g_closing = true;
            g_cancel = true;
            answerLocked(-1);
            LightLock_Unlock(&g_stateLock);
            break;
        }
        handleRunKeys(keys);
    }

    if (worker != NULL)
    {
        threadJoin(worker, U64_MAX);
        threadFree(worker);
    }

    LightLock_Lock(&g_stateLock);
    g_shared.mode = SCREEN_MENU;
    g_shared.debugRun = false;
    LightLock_Unlock(&g_stateLock);
}

void setActivity(const std::string &provider, const std::string &entry)
{
    LightLock_Lock(&g_stateLock);
    g_shared.provider = provider;
    g_shared.entry = entry;
    LightLock_Unlock(&g_stateLock);
}

void setLastResult(const std::string &text, bool ok)
{
    LightLock_Lock(&g_stateLock);
    g_shared.lastResult = text;
    g_shared.lastResultOk = ok;
    LightLock_Unlock(&g_stateLock);
}

bool askSaveDebugLog(unsigned kilobytes, bool trimmed)
{
    LightLock_Lock(&g_stateLock);
    g_shared.logKilobytes = kilobytes;
    g_shared.logTrimmed = trimmed;
    LightLock_Unlock(&g_stateLock);
    return ask(SCREEN_SAVE_LOG) == 1;
}

// ---------------------------------------------------------------------------
// WorkerSyncUi
// ---------------------------------------------------------------------------
WorkerSyncUi::WorkerSyncUi() : _cancelled(false), _announced(false), _applyAllSkip(false) {}

bool WorkerSyncUi::cancelRequested()
{
    if (!_cancelled)
    {
        LightLock_Lock(&g_stateLock);
        _cancelled = g_cancel;
        LightLock_Unlock(&g_stateLock);
        if (_cancelled && !_announced)
        {
            printf("  -> Cancellation requested\n");
            _announced = true;
        }
    }
    return _cancelled;
}

void WorkerSyncUi::requestCancel()
{
    _cancelled = true;
    LightLock_Lock(&g_stateLock);
    g_cancel = true;
    g_shared.cancelling = true;
    LightLock_Unlock(&g_stateLock);
}

ConflictChoice WorkerSyncUi::resolveConflict(const std::string &localPath,
                                             const std::vector<std::string> &sides)
{
    // "For all" remembers a side by name, so it answers every later conflict
    // that side is part of; one it is not part of is asked again.
    if (_applyAllSkip)
        return ConflictChoice::skip();
    for (size_t i = 0; i < sides.size() && !_applyAllSide.empty(); i++)
        if (sides[i] == _applyAllSide)
            return ConflictChoice::keep((int)i);

    LightLock_Lock(&g_stateLock);
    g_shared.conflictPath = localPath;
    g_shared.conflictSides = sides;
    g_shared.conflictPick = 0;
    g_shared.applyToAll = false;
    LightLock_Unlock(&g_stateLock);

    int answer = ask(SCREEN_CONFLICT);
    if (answer != ANSWER_SKIP && (answer < 0 || answer >= (int)sides.size()))
    {
        // The engine says "Sync cancelled" itself; announcing the request
        // as well would print the same thing twice.
        _cancelled = _announced = true;
        return ConflictChoice::cancel();
    }

    LightLock_Lock(&g_stateLock);
    bool forAll = g_shared.applyToAll;
    LightLock_Unlock(&g_stateLock);
    if (answer == ANSWER_SKIP)
    {
        _applyAllSkip = forAll;
        return ConflictChoice::skip();
    }
    if (forAll)
        _applyAllSide = sides[answer];
    return ConflictChoice::keep(answer);
}

} // namespace frontend
