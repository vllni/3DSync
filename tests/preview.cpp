// Renders every screen of the on-device UI to PNG files on the host, so a
// layout change can be looked at without a console:
//
//   make -C tests preview      ->  tests/build/preview/*.png
//
// Each image is the top screen above the bottom one, at 2x, as on a 3DS.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include "../source/ui/canvas.h"
#include "../source/ui/logbuffer.h"
#include "../source/ui/screens.h"
#include "../source/utils/console.h"

using namespace ui;

// ---------------------------------------------------------------------------
// A minimal PNG writer: stored (uncompressed) deflate blocks, so no zlib.
// ---------------------------------------------------------------------------
static uint32_t crcTable[256];

static void initCrc()
{
    for (uint32_t n = 0; n < 256; n++)
    {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
}

static uint32_t crc(const std::vector<uint8_t> &d, size_t from)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = from; i < d.size(); i++)
        c = crcTable[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void be32(std::vector<uint8_t> &d, uint32_t v)
{
    for (int i = 3; i >= 0; i--)
        d.push_back((uint8_t)(v >> (i * 8)));
}

static void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
    be32(out, (uint32_t)data.size());
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, crc(out, start));
}

static bool writePng(const std::string &path, int w, int h, const std::vector<uint8_t> &rgb)
{
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; y++)
    {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + (size_t)y * w * 3, rgb.begin() + (size_t)(y + 1) * w * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw)
    {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t i = 0; i < raw.size(); i += 65535)
    {
        size_t n = raw.size() - i < 65535 ? raw.size() - i : 65535;
        z.push_back(i + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n);
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n);
        z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    }
    be32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", std::vector<uint8_t>());

    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

static void save(const std::string &name, const Canvas &top, const Canvas &bottom)
{
    const int scale = 2, W = TOP_W * scale, H = (TOP_H + BOTTOM_H) * scale;
    std::vector<uint8_t> rgb((size_t)W * H * 3, 0);
    auto put = [&](const Canvas &c, int ox, int oy) {
        for (int y = 0; y < c.height(); y++)
            for (int x = 0; x < c.width(); x++)
            {
                Color p = c.pixels()[y * c.width() + x];
                uint8_t r = (uint8_t)((p >> 11) * 255 / 31), g = (uint8_t)(((p >> 5) & 63) * 255 / 63),
                        b = (uint8_t)((p & 31) * 255 / 31);
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                    {
                        size_t i = ((size_t)(oy + y * scale + dy) * W + ox + x * scale + dx) * 3;
                        rgb[i] = r;
                        rgb[i + 1] = g;
                        rgb[i + 2] = b;
                    }
            }
    };
    put(top, 0, 0);
    put(bottom, (TOP_W - BOTTOM_W) * scale / 2, TOP_H * scale);
    std::string path = "build/preview/" + name + ".png";
    if (writePng(path, W, H, rgb))
        printf("wrote tests/%s\n", path.c_str());
    else
        printf("cannot write %s\n", path.c_str());
}

static void fillLog(LogBuffer &log, bool debug, int extra)
{
    std::string s =
        "Initializing components...\n\n"
        "\n=== Dropbox ===\n"
        CONSOLE_YELLOW "  Dropbox support is EXPERIMENTAL." CONSOLE_RESET "\n"
        "  It has seen far less testing than Drive, so\n  please keep a backup of your saves.\n"
        "  Bugs and feedback are very welcome at\n"
        CONSOLE_CYAN "  github.com/vllni/3DSync/issues" CONSOLE_RESET "\n\n"
        "\nSyncing [/3ds/Checkpoint/saves]\n"
        "Listing Dropbox:Checkpoint/saves\n"
        "Listing SMB:Checkpoint/saves\n";
    if (debug)
        s += CONSOLE_MAGENTA "dbg: " "POST https://api.dropboxapi.com/2/files/list_folder -> 200 in 412 ms, 2318 bytes of response body" CONSOLE_RESET "\n";
    for (int i = 0; i < extra; i++)
        s += CONSOLE_MAGENTA "dbg: " "Pok\xC3\xA9mon Y/main: local unchanged, remote unchanged \xE2\x80\x94 skip" CONSOLE_RESET "\n";
    s += "\n  *** CONFLICT:\n  /3ds/Checkpoint/saves/0x0055D Pok\xC3\xA9mon Y/main\n"
         "  Changed on 3DS and Dropbox.\n\n";
    log.append(s.data(), s.size());
}

int main()
{
    initCrc();
    Canvas top(TOP_W, TOP_H), bottom(BOTTOM_W, BOTTOM_H);

    LogBuffer log(LOG_COLUMNS, 1500);
    LogScroll scroll(LOG_ROWS);
    TopModel tm;
    tm.debugRun = false;
    tm.busy = false;

    BottomModel m;
    m.starting = false;
    m.configLoaded = true;
    m.remotes = "Dropbox, SMB";
    m.pathCount = 3;
    m.logVisible = LOG_ROWS;
    auto syncLog = [&]() {
        scroll.update(log);
        m.logFirst = log.firstRow();
        m.logEnd = log.endRow();
        m.logTop = scroll.top();
    };

    const char *hello = "Initializing components...\n\n";
    log.append(hello, strlen(hello));
    syncLog();
    drawTop(top, tm, log, scroll);
    drawBottom(bottom, m, bottomButtons(m));
    save("1-menu", top, bottom);

    m.heldKeys = K_A;
    drawBottom(bottom, m, bottomButtons(m));
    save("1b-menu-a-held", top, bottom);
    m.heldKeys = 0;

    m.fileFilter = ".sav .bin .dat";
    m.lastResult = "2 up, 1 down";
    drawBottom(bottom, m, bottomButtons(m));
    save("1d-menu-filter", top, bottom);
    m.fileFilter = "";
    m.lastResult = "";

    // Startup with problems in 3DSync.ini, scrolled a little way down.
    {
        BottomModel issues = m;
        issues.mode = SCREEN_CONFIG_ISSUES;
        issues.configIssueCount = 4;
        issues.configIssueLines = {
            "A remote is missing a setting:", "  [smb:enc]", "  Add Share= (the share name).",
            "A remote has no login:", "  [dropbox]", "  Log in again with the configurator",
            "  and copy its RefreshToken= here.", "Role= is neither sync nor backup in",
            "  [ftp]", "  It syncs both ways until that is fixed.",
            "Only= names a path that is not configured:", "  savs", "  in [smb:backup]",
            "  Use a name from [Paths]."};
        drawBottom(bottom, issues, bottomButtons(issues));
        save("1e-config-issues", top, bottom);
        issues.configScroll = configScrollTo(issues, 6, -1);
        issues.logFirst = 0;
        issues.logEnd = configIssueRows(issues).size();
        issues.logTop = issues.configScroll;
        issues.logVisible = CONFIG_ISSUE_ROWS;
        drawBottom(bottom, issues, bottomButtons(issues));
        save("1f-config-issues-scrolled", top, bottom);
        BottomModel menu = m;
        menu.configIssueCount = 4;
        drawBottom(bottom, menu, bottomButtons(menu));
        save("1g-menu-check-config", top, bottom);
    }

    BottomModel none;
    none.starting = false;
    drawBottom(bottom, none, bottomButtons(none));
    save("1c-menu-no-config", top, bottom);

    log.clear();
    fillLog(log, false, 0);
    syncLog();
    tm.busy = true;
    m.mode = SCREEN_RUNNING;
    m.provider = "Dropbox";
    m.entry = "/3ds/Checkpoint/saves";
    m.elapsedSeconds = 83;
    m.spinnerFrame = 3;
    drawTop(top, tm, log, scroll);
    drawBottom(bottom, m, bottomButtons(m));
    save("2-running", top, bottom);

    m.cancelling = true;
    drawBottom(bottom, m, bottomButtons(m));
    save("2b-cancelling", top, bottom);
    m.cancelling = false;

    log.clear();
    fillLog(log, true, 40);
    tm.debugRun = true;
    m.debugRun = true;
    scroll.scrollToEnd(log);
    scroll.scrollBy(log, -25);
    syncLog();
    m.mode = SCREEN_CONFLICT;
    m.conflictPath = "/3ds/Checkpoint/saves/0x0055D Pok\xC3\xA9mon Y/main";
    m.conflictSides = {"3DS", "Dropbox"};
    m.heldKeys = K_DUP;
    drawTop(top, tm, log, scroll);
    drawBottom(bottom, m, bottomButtons(m));
    save("3-conflict-scrolled", top, bottom);

    m.heldKeys = 0;
    m.applyToAll = true;
    scroll.scrollToEnd(log);
    syncLog();
    drawTop(top, tm, log, scroll);
    drawBottom(bottom, m, bottomButtons(m));
    save("3b-conflict-apply-all", top, bottom);

    m.applyToAll = false;
    m.conflictSides = {"3DS", "Dropbox", "SMB", "WebDAV"};
    m.conflictPick = 2;
    drawBottom(bottom, m, bottomButtons(m));
    save("3c-conflict-four-sides", top, bottom);

    // More than fit: the list follows the highlight, a row is being tapped.
    m.conflictSides = {"3DS", "Dropbox", "Drive", "SMB", "FTP", "WebDAV"};
    m.conflictPick = 5;
    m.heldKeys = K_LIST_ROW0 << 1;
    drawBottom(bottom, m, bottomButtons(m));
    save("3e-conflict-six-sides", top, bottom);
    m.heldKeys = 0;

    m.conflictSides = {"Drive", "WebDAV"};
    m.conflictPick = 0;
    drawBottom(bottom, m, bottomButtons(m));
    save("3d-conflict-two-remotes", top, bottom);

    tm.busy = false;
    m.mode = SCREEN_SAVE_LOG;
    m.logKilobytes = 42;
    drawTop(top, tm, log, scroll);
    drawBottom(bottom, m, bottomButtons(m));
    save("4-save-log", top, bottom);

    m.mode = SCREEN_MENU;
    m.debugRun = false;
    m.lastResult = "2 uploaded, 1 downloaded";
    drawBottom(bottom, m, bottomButtons(m));
    save("5-menu-after", top, bottom);
    return 0;
}
