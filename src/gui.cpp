// Win32 window for the trainer: wallet, free teleport / rewards, and a navigator to every
// country flag, landing site and your own marks, with a compass window that guides you.
// Built twice from this file: the full trainer, and (NAVIGATOR_ONLY) a read-only navigator.
// No third-party libraries.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <commctrl.h>

#include "decimal.hpp"
#include "game.hpp"
#include "storage.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifndef TRAINER_VERSION
#define TRAINER_VERSION "dev"
#endif

#ifdef NAVIGATOR_ONLY
constexpr bool kNavigatorOnly = true;   // never writes to the game
#else
constexpr bool kNavigatorOnly = false;
#endif

namespace {

// ---------------------------------------------------------------- text helpers

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring wfmt(const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return widen(buf);
}

bool g_russian = false;

// English / Russian UI text.
std::wstring T(const char* en, const char* ru) { return widen(g_russian ? ru : en); }

std::wstring get_text(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

void set_text(HWND h, const std::wstring& s) {
    if (get_text(h) != s) {  // avoids flicker while the timer refreshes
        SetWindowTextW(h, s.c_str());
    }
}

void copy_to_clipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = GlobalLock(mem)) {
            std::memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
                GlobalFree(mem);
            }
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// ------------------------------------------------------------------ geometry

using ms::Vec3;

Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(const Vec3& a) {
    const float l = length(a);
    return l > 1e-9f ? a * (1.0f / l) : Vec3{};
}

constexpr double kPi = 3.14159265358979323846;

// The player's frame on the Moon's surface: "up" is away from the centre, forward the facing direction.
struct Frame {
    Vec3 up, fwd, right;
    float radius = 0;
    bool has_heading = false;
};

bool make_frame(const Vec3& pos, const Vec3& heading, Frame& f) {
    f.radius = length(pos);
    if (f.radius < 1.0f) {
        return false;
    }
    f.up = normalized(pos);
    Vec3 fwd = heading - f.up * dot(heading, f.up);
    f.has_heading = length(fwd) > 1e-4f;
    f.fwd = normalized(fwd);
    f.right = cross(f.up, f.fwd);  // Unity: cross(up, forward) points to the player's right
    return true;
}

struct Metrics {
    double distance = -1;  // metres along the surface, -1 unknown
    double relative = 0;   // degrees, positive = to the right of the player's heading
    bool has_direction = false;
};

Metrics measure(const Frame& f, const Vec3& dir) {
    Metrics m;
    const float c = std::max(-1.0f, std::min(1.0f, dot(f.up, dir)));
    m.distance = static_cast<double>(f.radius) * std::acos(c);
    const Vec3 t = normalized(dir - f.up * dot(dir, f.up));
    if (f.has_heading && length(t) > 0.5f) {
        m.relative = std::atan2(static_cast<double>(dot(t, f.right)), static_cast<double>(dot(t, f.fwd))) *
                     180.0 / kPi;
        m.has_direction = true;
    }
    return m;
}

// ------------------------------------------------------------------- places

enum class Kind { Flag, Site, Mark };

// Anything you can navigate to: a country flag, a landing site or a mark of your own.
struct Place {
    Kind kind = Kind::Flag;
    int landmark_index = -1;  // index into the game's landmark arrays; -1 for marks
    int number = 0;           // value of the "#" column
    std::string name;
    std::string key;          // persistent identity for the "visited" ticks
    float lat = 0, lon = 0;
    Vec3 dir;
};

void lat_lon_of(const Vec3& d, float& lat, float& lon) {
    lat = static_cast<float>(std::asin(std::max(-1.0f, std::min(1.0f, d.y))) * 180.0 / kPi);
    lon = static_cast<float>(std::atan2(static_cast<double>(d.x), -static_cast<double>(d.z)) * 180.0 / kPi);
}

// ------------------------------------------------------------------- the app

constexpr UINT WM_ATTACHED = WM_APP + 1;
constexpr UINT WM_ATTACH_FAILED = WM_APP + 2;
constexpr UINT_PTR kTimer = 1;

enum Id {
    IDC_STATUS = 100, IDC_RESCAN, IDC_GRP_WALLET, IDC_BALANCE, IDC_LBL_AMOUNT, IDC_AMOUNT, IDC_SET,
    IDC_FREEZE, IDC_GRP_GAME, IDC_FREETP, IDC_TPINFO, IDC_LBL_FLAG, IDC_FLAGREW, IDC_LBL_SITE,
    IDC_SITEREW, IDC_APPLY, IDC_RESTORE, IDC_GRP_PLAYER, IDC_PLAYER, IDC_WARN, IDC_FILTER,
    IDC_SEARCH, IDC_LIST, IDC_SELINFO, IDC_COPY, IDC_GUIDE, IDC_VISITED, IDC_MARKNAME, IDC_MARKHERE,
    IDC_MARKDEL
};

enum Column { COL_NUM, COL_TYPE, COL_NAME, COL_SEEN, COL_LAT, COL_LON, COL_DIST, COL_DIR, COL_COUNT };

// Writes the held amount into the wallet ~20 times a second.
struct Freezer {
    std::mutex mutex;
    std::shared_ptr<ms::Game> game;
    ms::Decimal value;
    bool on = false;
    std::atomic<bool> quit{false};
    std::thread thread;

    void start() {
        thread = std::thread([this] {
            while (!quit) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (on && game) {
                        game->write_balance(value);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    void stop() {
        quit = true;
        if (thread.joinable()) {
            thread.join();
        }
    }
};

struct App {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    int dpi = 96;

    HWND status = nullptr, rescan = nullptr, grp_wallet = nullptr, balance = nullptr, lbl_amount = nullptr,
         amount = nullptr, set_btn = nullptr, freeze = nullptr, grp_game = nullptr, freetp = nullptr,
         tpinfo = nullptr, lbl_flag = nullptr, flagrew = nullptr, lbl_site = nullptr, siterew = nullptr,
         apply = nullptr, restore = nullptr, grp_player = nullptr, player = nullptr, warn = nullptr,
         filter = nullptr, search = nullptr, list = nullptr, selinfo = nullptr, copy = nullptr,
         guide = nullptr, visited_btn = nullptr, markname = nullptr, markhere = nullptr, markdel = nullptr;

    std::shared_ptr<ms::Game> game;
    bool attaching = false;
    Freezer freezer;
    ms::Storage storage;

    bool have_originals = false;
    ms::Decimal orig_tp, orig_flag, orig_site;

    std::vector<Place> places;          // landmarks from the game + your marks
    std::vector<Metrics> metrics;       // per place
    std::vector<uint8_t> raised;        // game: which landmarks are already raised
    Vec3 pos, heading;
    bool have_player = false;

    int sort_column = COL_DIST;
    bool sort_ascending = true;
    ULONGLONG last_sort = 0;
    bool editing_rewards = false;

    // compass window
    HWND compass = nullptr;
    bool has_target = false;
    Place target;
    HFONT compass_big = nullptr, compass_small = nullptr;

    int S(int v) const { return MulDiv(v, dpi, 96); }
};

App* g_app = nullptr;  // list-view sort callback and the compass window

std::wstring decimal_text(const ms::Decimal& d) { return widen(ms::to_string(d)); }

std::wstring distance_text(double metres) {
    if (metres < 0) {
        return L"-";
    }
    return metres < 1000 ? wfmt("%.0f m", metres) : wfmt("%.2f km", metres / 1000.0);
}

std::wstring direction_text(const Metrics& m) {
    if (!m.has_direction) {
        return L"-";
    }
    static const wchar_t* const arrows[] = {L"↑", L"↗", L"→", L"↘", L"↓",
                                            L"↙", L"←", L"↖"};
    int octant = static_cast<int>(std::lround(m.relative / 45.0));
    octant = ((octant % 8) + 8) % 8;
    return std::wstring(arrows[octant]) + wfmt(" %+.0f°", m.relative);
}

bool game_raised(const App& a, const Place& p) {
    return p.landmark_index >= 0 && static_cast<size_t>(p.landmark_index) < a.raised.size() &&
           a.raised[static_cast<size_t>(p.landmark_index)] != 0;
}

bool manually_visited(const App& a, const Place& p) { return a.storage.visited.count(p.key) > 0; }

bool is_visited(const App& a, const Place& p) { return game_raised(a, p) || manually_visited(a, p); }

std::wstring seen_text(const App& a, const Place& p) {
    if (game_raised(a, p)) {
        return L"✓";          // raised in the game
    }
    return manually_visited(a, p) ? L"●" : L"";  // ticked by hand
}

void rebuild_places(App& a) {
    a.places.clear();
    if (a.game) {
        for (const ms::Landmark& lm : a.game->landmarks()) {
            Place p;
            p.kind = lm.flag ? Kind::Flag : Kind::Site;
            p.landmark_index = lm.index;
            p.number = lm.index;
            p.name = lm.name;
            p.key = lm.id;
            p.lat = lm.lat;
            p.lon = lm.lon;
            p.dir = lm.dir;
            a.places.push_back(std::move(p));
        }
    }
    int n = 1000;
    for (const ms::Mark& m : a.storage.marks) {
        Place p;
        p.kind = Kind::Mark;
        p.number = ++n;
        p.name = m.name;
        p.key = "mark:" + m.name;
        p.dir = normalized(m.dir);
        lat_lon_of(p.dir, p.lat, p.lon);
        a.places.push_back(std::move(p));
    }
    a.metrics.assign(a.places.size(), Metrics{});
}

// ------------------------------------------------------------------ controls

HWND make(App& a, const wchar_t* cls, const std::wstring& text, DWORD style, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, a.hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                           nullptr);
}

BOOL CALLBACK set_font_proc(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
    return TRUE;
}

void place_at(HWND h, int x, int y, int w, int hgt) { MoveWindow(h, x, y, w, hgt, TRUE); }

void layout(App& a) {
    RECT rc;
    GetClientRect(a.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = a.S(10);
    const int left_w = a.S(350);
    const int row = a.S(24);

    place_at(a.status, m, a.S(12), W - a.S(150), a.S(20));
    place_at(a.rescan, W - a.S(130) - m + a.S(10), a.S(8), a.S(120), a.S(28));

    int y = a.S(44);
    if (!kNavigatorOnly) {
        // wallet
        place_at(a.grp_wallet, m, y, left_w, a.S(150));
        place_at(a.balance, m + a.S(12), y + a.S(22), left_w - a.S(24), a.S(20));
        place_at(a.lbl_amount, m + a.S(12), y + a.S(52), left_w - a.S(24), a.S(18));
        place_at(a.amount, m + a.S(12), y + a.S(72), a.S(170), row);
        place_at(a.set_btn, m + a.S(192), y + a.S(71), left_w - a.S(204), a.S(26));
        place_at(a.freeze, m + a.S(12), y + a.S(110), left_w - a.S(24), a.S(22));
        y += a.S(160);
        // game settings
        place_at(a.grp_game, m, y, left_w, a.S(200));
        place_at(a.freetp, m + a.S(12), y + a.S(22), left_w - a.S(24), a.S(22));
        place_at(a.tpinfo, m + a.S(30), y + a.S(46), left_w - a.S(42), a.S(20));
        place_at(a.lbl_flag, m + a.S(12), y + a.S(80), a.S(150), a.S(20));
        place_at(a.flagrew, m + a.S(170), y + a.S(76), left_w - a.S(182), row);
        place_at(a.lbl_site, m + a.S(12), y + a.S(112), a.S(150), a.S(20));
        place_at(a.siterew, m + a.S(170), y + a.S(108), left_w - a.S(182), row);
        place_at(a.apply, m + a.S(12), y + a.S(150), (left_w - a.S(36)) / 2, a.S(28));
        place_at(a.restore, m + a.S(24) + (left_w - a.S(36)) / 2, y + a.S(150), (left_w - a.S(36)) / 2, a.S(28));
        y += a.S(210);
    }
    // player
    place_at(a.grp_player, m, y, left_w, a.S(86));
    place_at(a.player, m + a.S(12), y + a.S(20), left_w - a.S(24), a.S(60));
    y += a.S(94);
    place_at(a.warn, m, y, left_w, H - y - m);

    // right side: navigator
    const int rx = m + left_w + m;
    const int rw = W - rx - m;
    const int gap = a.S(8);
    const int bw = (rw - 2 * gap) / 3;
    place_at(a.filter, rx, a.S(44), a.S(160), a.S(240));
    place_at(a.search, rx + a.S(170), a.S(44), rw - a.S(170), row);
    place_at(a.list, rx, a.S(76), rw, H - a.S(76) - a.S(150));
    place_at(a.selinfo, rx, H - a.S(144), rw, a.S(52));
    place_at(a.guide, rx, H - a.S(86), bw, a.S(30));
    place_at(a.visited_btn, rx + bw + gap, H - a.S(86), bw, a.S(30));
    place_at(a.copy, rx + 2 * (bw + gap), H - a.S(86), bw, a.S(30));
    place_at(a.markname, rx, H - a.S(46), rw - 2 * (bw + gap), row + a.S(2));
    place_at(a.markhere, rx + rw - 2 * bw - gap, H - a.S(48), bw, a.S(30));
    place_at(a.markdel, rx + rw - bw, H - a.S(48), bw, a.S(30));
}

void add_columns(App& a) {
    const struct {
        const char* en;
        const char* ru;
        int width;
        int fmt;
    } cols[COL_COUNT] = {
        {"#", "#", 44, LVCFMT_RIGHT},           {"Type", "Тип", 62, LVCFMT_LEFT},
        {"Name", "Название", 190, LVCFMT_LEFT}, {"Seen", "Побывал", 66, LVCFMT_CENTER},
        {"Lat", "Шир.", 68, LVCFMT_RIGHT},      {"Lon", "Долг.", 72, LVCFMT_RIGHT},
        {"Distance", "Дистанция", 84, LVCFMT_RIGHT}, {"Direction", "Направление", 96, LVCFMT_LEFT},
    };
    for (int i = 0; i < COL_COUNT; ++i) {
        std::wstring title = T(cols[i].en, cols[i].ru);
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = &title[0];
        c.cx = a.S(cols[i].width);
        c.fmt = cols[i].fmt;
        SendMessageW(a.list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&c));
    }
}

void create_controls(App& a) {
    a.status = make(a, L"STATIC", L"", SS_LEFT, IDC_STATUS);
    a.rescan = make(a, L"BUTTON", T("Rescan game", "Найти игру заново"), BS_PUSHBUTTON | WS_TABSTOP, IDC_RESCAN);

    if (!kNavigatorOnly) {
        a.grp_wallet = make(a, L"BUTTON", T("Team wallet", "Кошелёк команды"), BS_GROUPBOX, IDC_GRP_WALLET);
        a.balance = make(a, L"STATIC", L"", SS_LEFT, IDC_BALANCE);
        a.lbl_amount = make(a, L"STATIC", T("Amount:", "Сумма:"), SS_LEFT, IDC_LBL_AMOUNT);
        a.amount = make(a, L"EDIT", L"10000", ES_AUTOHSCROLL | WS_TABSTOP, IDC_AMOUNT, WS_EX_CLIENTEDGE);
        a.set_btn = make(a, L"BUTTON", T("Set balance", "Установить"), BS_PUSHBUTTON | WS_TABSTOP, IDC_SET);
        a.freeze = make(a, L"BUTTON", T("Hold this amount (freeze)", "Держать эту сумму (заморозить)"),
                        BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FREEZE);

        a.grp_game = make(a, L"BUTTON", T("Teleport and rewards", "Телепорт и награды"), BS_GROUPBOX, IDC_GRP_GAME);
        a.freetp = make(a, L"BUTTON", T("Free in-game teleport (price 0)", "Бесплатный телепорт игры (цена 0)"),
                        BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FREETP);
        a.tpinfo = make(a, L"STATIC", L"", SS_LEFT, IDC_TPINFO);
        a.lbl_flag = make(a, L"STATIC", T("Reward per flag:", "Награда за флаг:"), SS_LEFT, IDC_LBL_FLAG);
        a.flagrew = make(a, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_FLAGREW, WS_EX_CLIENTEDGE);
        a.lbl_site = make(a, L"STATIC", T("Reward per site:", "Награда за метку:"), SS_LEFT, IDC_LBL_SITE);
        a.siterew = make(a, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SITEREW, WS_EX_CLIENTEDGE);
        a.apply = make(a, L"BUTTON", T("Apply rewards", "Применить награды"), BS_PUSHBUTTON | WS_TABSTOP, IDC_APPLY);
        a.restore = make(a, L"BUTTON", T("Restore originals", "Вернуть как было"), BS_PUSHBUTTON | WS_TABSTOP, IDC_RESTORE);
    }

    a.grp_player = make(a, L"BUTTON", T("Player", "Игрок"), BS_GROUPBOX, IDC_GRP_PLAYER);
    a.player = make(a, L"STATIC", L"", SS_LEFT, IDC_PLAYER);
    a.warn = make(a, L"STATIC",
                  kNavigatorOnly
                      ? T("Navigator: read-only. It only reads the game's memory and never changes the game.\n\n"
                          "Your own marks and ticks are kept in %APPDATA%\\MoonSweepersTrainer.",
                          "Навигатор: только чтение. Он лишь читает память игры и ничего в ней не меняет.\n\n"
                          "Твои отметки хранятся в %APPDATA%\\MoonSweepersTrainer.")
                      : T("This game is co-op. Changes are meant for your own session only; do not use them in "
                          "other players' crews.\n\nYour own marks and ticks are kept in "
                          "%APPDATA%\\MoonSweepersTrainer.",
                          "Игра кооперативная. Используй изменения только в своей сессии, не в чужих экипажах.\n\n"
                          "Твои отметки хранятся в %APPDATA%\\MoonSweepersTrainer."),
                  SS_LEFT, IDC_WARN);

    a.filter = make(a, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_FILTER);
    const std::wstring modes[] = {T("All", "Все"),           T("Flags", "Флаги"),
                                  T("Sites", "Метки"),       T("My marks", "Мои точки"),
                                  T("Not visited", "Не был"), T("Visited", "Побывал")};
    for (const std::wstring& m : modes) {
        SendMessageW(a.filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m.c_str()));
    }
    SendMessageW(a.filter, CB_SETCURSEL, 0, 0);
    a.search = make(a, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SEARCH, WS_EX_CLIENTEDGE);
    SendMessageW(a.search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(T("Search by name...", "Поиск по названию...").c_str()));

    a.list = make(a, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_LIST,
                  WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(a.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    add_columns(a);
    a.selinfo = make(a, L"STATIC", L"", SS_LEFT, IDC_SELINFO);
    a.guide = make(a, L"BUTTON", T("Guide me there", "Вести к цели"), BS_PUSHBUTTON | WS_TABSTOP, IDC_GUIDE);
    a.visited_btn = make(a, L"BUTTON", T("Toggle \"visited\"", "Отметить \"побывал\""), BS_PUSHBUTTON | WS_TABSTOP, IDC_VISITED);
    a.copy = make(a, L"BUTTON", T("Copy coordinates", "Копировать координаты"), BS_PUSHBUTTON | WS_TABSTOP, IDC_COPY);
    a.markname = make(a, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_MARKNAME, WS_EX_CLIENTEDGE);
    SendMessageW(a.markname, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(T("Name of your mark...", "Название своей точки...").c_str()));
    a.markhere = make(a, L"BUTTON", T("Mark my position", "Отметить моё место"), BS_PUSHBUTTON | WS_TABSTOP, IDC_MARKHERE);
    a.markdel = make(a, L"BUTTON", T("Delete my mark", "Удалить мою точку"), BS_PUSHBUTTON | WS_TABSTOP, IDC_MARKDEL);

    EnumChildWindows(a.hwnd, set_font_proc, reinterpret_cast<LPARAM>(a.font));
}

// ------------------------------------------------------------------ the list

int filter_mode(App& a) { return static_cast<int>(SendMessageW(a.filter, CB_GETCURSEL, 0, 0)); }

bool contains_nocase(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }
    std::wstring h = hay, n = needle;
    CharLowerBuffW(&h[0], static_cast<DWORD>(h.size()));
    CharLowerBuffW(&n[0], static_cast<DWORD>(n.size()));
    return h.find(n) != std::wstring::npos;
}

std::wstring kind_text(Kind k) {
    switch (k) {
        case Kind::Flag: return T("Flag", "Флаг");
        case Kind::Site: return T("Site", "Метка");
        case Kind::Mark: return T("Mine", "Моя");
    }
    return L"";
}

bool passes_filter(const App& a, const Place& p, int mode, const std::wstring& needle) {
    switch (mode) {
        case 1: if (p.kind != Kind::Flag) return false; break;
        case 2: if (p.kind != Kind::Site) return false; break;
        case 3: if (p.kind != Kind::Mark) return false; break;
        case 4: if (is_visited(a, p)) return false; break;
        case 5: if (!is_visited(a, p)) return false; break;
        default: break;
    }
    return contains_nocase(widen(p.name), needle);
}

void rebuild_list(App& a) {
    ListView_DeleteAllItems(a.list);
    const int mode = filter_mode(a);
    const std::wstring needle = get_text(a.search);

    SendMessageW(a.list, WM_SETREDRAW, FALSE, 0);
    int row = 0;
    for (size_t i = 0; i < a.places.size(); ++i) {
        const Place& p = a.places[i];
        if (!passes_filter(a, p, mode, needle)) {
            continue;
        }
        std::wstring num = p.kind == Kind::Mark ? std::wstring(L"*") : std::to_wstring(p.number);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = &num[0];
        item.lParam = static_cast<LPARAM>(i);
        SendMessageW(a.list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));

        auto sub = [&](int col, std::wstring text) {
            ListView_SetItemText(a.list, row, col, &text[0]);
        };
        sub(COL_TYPE, kind_text(p.kind));
        sub(COL_NAME, widen(p.name));
        sub(COL_SEEN, seen_text(a, p));
        sub(COL_LAT, wfmt("%.3f", p.lat));
        sub(COL_LON, wfmt("%.3f", p.lon));
        ++row;
    }
    SendMessageW(a.list, WM_SETREDRAW, TRUE, 0);
}

int CALLBACK compare_rows(LPARAM l1, LPARAM l2, LPARAM) {
    App& a = *g_app;
    const size_t i = static_cast<size_t>(l1), j = static_cast<size_t>(l2);
    if (i >= a.places.size() || j >= a.places.size()) {
        return 0;
    }
    const Place& pi = a.places[i];
    const Place& pj = a.places[j];
    int r = 0;
    switch (a.sort_column) {
        case COL_NUM: r = pi.number - pj.number; break;
        case COL_TYPE: r = static_cast<int>(pi.kind) - static_cast<int>(pj.kind); break;
        case COL_NAME: r = pi.name.compare(pj.name); break;
        case COL_SEEN: r = static_cast<int>(is_visited(a, pi)) - static_cast<int>(is_visited(a, pj)); break;
        case COL_LAT: r = pi.lat < pj.lat ? -1 : (pi.lat > pj.lat ? 1 : 0); break;
        case COL_LON: r = pi.lon < pj.lon ? -1 : (pi.lon > pj.lon ? 1 : 0); break;
        case COL_DIST:
        case COL_DIR: {
            const double di = i < a.metrics.size() ? a.metrics[i].distance : -1;
            const double dj = j < a.metrics.size() ? a.metrics[j].distance : -1;
            r = di < dj ? -1 : (di > dj ? 1 : 0);
            break;
        }
        default: break;
    }
    return a.sort_ascending ? r : -r;
}

void sort_list(App& a) {
    g_app = &a;
    ListView_SortItems(a.list, compare_rows, 0);
    a.last_sort = GetTickCount64();
}

void update_metrics(App& a) {
    a.metrics.assign(a.places.size(), Metrics{});
    Frame f;
    if (!a.have_player || !make_frame(a.pos, a.heading, f)) {
        return;
    }
    for (size_t i = 0; i < a.places.size(); ++i) {
        a.metrics[i] = measure(f, a.places[i].dir);
    }
}

void refresh_list_texts(App& a) {
    const int n = ListView_GetItemCount(a.list);
    for (int row = 0; row < n; ++row) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (!ListView_GetItem(a.list, &item)) {
            continue;
        }
        const size_t idx = static_cast<size_t>(item.lParam);
        if (idx >= a.metrics.size()) {
            continue;
        }
        std::wstring d = distance_text(a.metrics[idx].distance);
        std::wstring dir = direction_text(a.metrics[idx]);
        std::wstring seen = seen_text(a, a.places[idx]);
        ListView_SetItemText(a.list, row, COL_DIST, &d[0]);
        ListView_SetItemText(a.list, row, COL_DIR, &dir[0]);
        ListView_SetItemText(a.list, row, COL_SEEN, &seen[0]);
    }
}

int selected_place(App& a) {
    const int row = ListView_GetNextItem(a.list, -1, LVNI_SELECTED);
    if (row < 0) {
        return -1;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(a.list, &item)) {
        return -1;
    }
    const int idx = static_cast<int>(item.lParam);
    return idx >= 0 && static_cast<size_t>(idx) < a.places.size() ? idx : -1;
}

std::wstring place_details(App& a, int idx) {
    const Place& p = a.places[static_cast<size_t>(idx)];
    // Approximate world position: direction * the player's current distance from the Moon's centre.
    const float r = a.have_player ? length(a.pos) : 2002.4f;
    std::wstring seen = game_raised(a, p) ? T("raised in the game", "поднят в игре")
                        : manually_visited(a, p) ? T("ticked by you", "отмечен тобой")
                                                 : T("not visited", "не посещён");
    return wfmt("%s - lat %.4f, lon %.4f\nworld ~ (%.1f, %.1f, %.1f)\n", p.name.c_str(), p.lat, p.lon,
                p.dir.x * r, p.dir.y * r, p.dir.z * r) + seen;
}

// -------------------------------------------------------------- compass window

constexpr const wchar_t* kCompassClass = L"MoonSweepersCompassWindow";

void paint_compass(HWND hwnd, App& a) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(RGB(22, 26, 34));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);

    Metrics m;
    bool known = false;
    if (a.game && a.has_target) {
        Vec3 pos, heading;
        Frame f;
        if (a.game->player(pos, heading) && make_frame(pos, heading, f)) {
            m = measure(f, a.target.dir);
            known = true;
        }
    }

    const bool arrived = known && m.distance < 15.0;
    const bool facing = known && m.has_direction && std::fabs(m.relative) < 10.0;
    const COLORREF accent = arrived ? RGB(120, 220, 120) : (facing ? RGB(120, 220, 120) :
                            (known && m.has_direction && std::fabs(m.relative) < 35.0 ? RGB(240, 200, 90)
                                                                                     : RGB(120, 170, 255)));

    // target name
    SelectObject(mem, a.compass_small);
    SetTextColor(mem, RGB(235, 238, 245));
    RECT title{a.S(10), a.S(8), W - a.S(10), a.S(34)};
    const std::wstring name = a.has_target ? widen(a.target.name) : T("No target", "Нет цели");
    DrawTextW(mem, name.c_str(), -1, &title, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    // compass circle and arrow
    const int cx = W / 2;
    const int cy = a.S(40) + (H - a.S(40) - a.S(86)) / 2;
    const int radius = std::min(W / 2 - a.S(16), (H - a.S(40) - a.S(86)) / 2 - a.S(4));
    HPEN ring = CreatePen(PS_SOLID, std::max(1, a.S(2)), RGB(70, 78, 96));
    HGDIOBJ old_pen = SelectObject(mem, ring);
    HGDIOBJ old_brush = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Ellipse(mem, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(mem, old_brush);
    SelectObject(mem, old_pen);
    DeleteObject(ring);

    if (known && !arrived && m.has_direction) {
        const double th = m.relative * kPi / 180.0;  // positive = clockwise on screen
        const double c = std::cos(th), s = std::sin(th);
        const double R = radius * 0.82;
        const double pts[4][2] = {{0, -R}, {-R * 0.42, R * 0.55}, {0, R * 0.22}, {R * 0.42, R * 0.55}};
        POINT poly[4];
        for (int i = 0; i < 4; ++i) {
            poly[i].x = cx + static_cast<LONG>(std::lround(pts[i][0] * c - pts[i][1] * s));
            poly[i].y = cy + static_cast<LONG>(std::lround(pts[i][0] * s + pts[i][1] * c));
        }
        HBRUSH ab = CreateSolidBrush(accent);
        HPEN ap = CreatePen(PS_SOLID, 1, accent);
        HGDIOBJ ob = SelectObject(mem, ab);
        HGDIOBJ op = SelectObject(mem, ap);
        Polygon(mem, poly, 4);
        SelectObject(mem, ob);
        SelectObject(mem, op);
        DeleteObject(ab);
        DeleteObject(ap);
    } else if (arrived) {
        SelectObject(mem, a.compass_big);
        SetTextColor(mem, accent);
        RECT r{cx - radius, cy - radius, cx + radius, cy + radius};
        const std::wstring here = T("You are here", "Ты на месте");
        DrawTextW(mem, here.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (!known) {
        SelectObject(mem, a.compass_small);
        SetTextColor(mem, RGB(170, 175, 190));
        RECT r{cx - radius, cy - radius, cx + radius, cy + radius};
        const std::wstring txt = T("No position", "Нет позиции");
        DrawTextW(mem, txt.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // distance and turn hint
    SelectObject(mem, a.compass_big);
    SetTextColor(mem, accent);
    RECT dist{a.S(10), H - a.S(84), W - a.S(10), H - a.S(40)};
    DrawTextW(mem, known ? distance_text(m.distance).c_str() : L"-", -1, &dist, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(mem, a.compass_small);
    SetTextColor(mem, RGB(200, 205, 218));
    std::wstring hint;
    if (known && m.has_direction && !arrived) {
        const double r = m.relative;
        if (std::fabs(r) < 10.0) {
            hint = T("straight ahead", "прямо по курсу");
        } else if (std::fabs(r) > 150.0) {
            hint = T("turn around", "развернись");
        } else if (r > 0) {
            hint = wfmt("%s %.0f°", g_russian ? "поверни направо" : "turn right", r);
        } else {
            hint = wfmt("%s %.0f°", g_russian ? "поверни налево" : "turn left", -r);
        }
    }
    RECT hr{a.S(10), H - a.S(38), W - a.S(10), H - a.S(8)};
    DrawTextW(mem, hint.c_str(), -1, &hr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK compass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = g_app;
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 50, nullptr);
            return 0;
        case WM_TIMER:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (a) {
                paint_compass(hwnd, *a);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            if (a && a->compass == hwnd) {
                a->compass = nullptr;
            }
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void open_compass(App& a, const Place& target) {
    a.target = target;
    a.has_target = true;
    const std::wstring title = T("Guide: ", "Навигатор: ") + widen(target.name);
    if (a.compass != nullptr) {
        SetWindowTextW(a.compass, title.c_str());
        InvalidateRect(a.compass, nullptr, FALSE);
        return;
    }
    const int w = a.S(300), h = a.S(380);
    const int x = GetSystemMetrics(SM_CXSCREEN) - w - a.S(24);
    // Topmost, no owner (so it stays up when the trainer window is minimised), does not steal focus.
    a.compass = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kCompassClass, title.c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, a.S(70), w, h, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (a.compass != nullptr) {
        ShowWindow(a.compass, SW_SHOWNOACTIVATE);
        UpdateWindow(a.compass);
    }
}

// ------------------------------------------------------------------ actions

void status(App& a, const std::wstring& text) { set_text(a.status, text); }

void start_attach(App& a) {
    if (a.attaching) {
        return;
    }
    a.attaching = true;
    status(a, T("Connecting to MoonSweepers... (a few seconds)", "Подключение к MoonSweepers... (несколько секунд)"));
    EnableWindow(a.rescan, FALSE);
    const HWND hwnd = a.hwnd;
    std::thread([hwnd] {
        std::string error;
        std::shared_ptr<ms::Game> game = ms::Game::attach(error);
        if (game) {
            auto* payload = new std::shared_ptr<ms::Game>(std::move(game));
            if (!PostMessageW(hwnd, WM_ATTACHED, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        } else {
            auto* payload = new std::string(error);
            if (!PostMessageW(hwnd, WM_ATTACH_FAILED, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        }
    }).detach();
}

bool parse_edit(HWND edit, ms::Decimal& out) {
    return ms::parse_decimal(narrow(get_text(edit)), out);
}

void set_freeze(App& a, bool on) {
    std::lock_guard<std::mutex> lock(a.freezer.mutex);
    a.freezer.on = on;
}

void on_attached(App& a, std::shared_ptr<ms::Game> game) {
    a.attaching = false;
    EnableWindow(a.rescan, TRUE);
    a.game = game;
    if (!kNavigatorOnly) {
        {
            std::lock_guard<std::mutex> lock(a.freezer.mutex);
            a.freezer.game = game;
        }
        if (!a.have_originals) {
            a.have_originals = game->read_setting(ms::Setting::TeleportPrice, a.orig_tp) &&
                               game->read_setting(ms::Setting::FlagReward, a.orig_flag) &&
                               game->read_setting(ms::Setting::SiteReward, a.orig_site);
        }
    }
    status(a, T("Connected. Places loaded: ", "Подключено. Загружено мест: ") +
                  std::to_wstring(game->landmarks().size()));
    game->read_raised(a.raised);
    rebuild_places(a);
    rebuild_list(a);
    a.last_sort = 0;
}

void restore_game_settings(App& a) {
    if (!kNavigatorOnly && a.game && a.have_originals && a.game->alive()) {
        a.game->write_setting(ms::Setting::TeleportPrice, a.orig_tp);
        a.game->write_setting(ms::Setting::FlagReward, a.orig_flag);
        a.game->write_setting(ms::Setting::SiteReward, a.orig_site);
    }
}

void on_timer(App& a) {
    if (!a.game) {
        return;
    }
    if (!a.game->alive()) {
        a.game.reset();
        if (!kNavigatorOnly) {
            std::lock_guard<std::mutex> lock(a.freezer.mutex);
            a.freezer.game.reset();
        }
        status(a, T("The game was closed. Start it and press \"Rescan game\".",
                    "Игра закрыта. Запусти её и нажми \"Найти игру заново\"."));
        return;
    }

    if (!kNavigatorOnly) {
        ms::Decimal d;
        if (a.game->read_balance(d)) {
            set_text(a.balance, T("Balance: ", "Баланс: ") + decimal_text(d));
        } else {
            set_text(a.balance, T("Balance: unavailable (rescan)", "Баланс недоступен (нажми \"Найти игру заново\")"));
        }
        ms::Decimal tp;
        if (a.game->read_setting(ms::Setting::TeleportPrice, tp)) {
            set_text(a.tpinfo, T("Current teleport price: ", "Текущая цена телепорта: ") + decimal_text(tp));
        }
        if (!a.editing_rewards && GetFocus() != a.flagrew && GetFocus() != a.siterew) {
            ms::Decimal fr, sr;
            if (a.game->read_setting(ms::Setting::FlagReward, fr)) {
                set_text(a.flagrew, decimal_text(fr));
            }
            if (a.game->read_setting(ms::Setting::SiteReward, sr)) {
                set_text(a.siterew, decimal_text(sr));
            }
        }
    }

    a.game->read_raised(a.raised);
    a.have_player = a.game->player(a.pos, a.heading);
    if (a.have_player) {
        float lat = 0, lon = 0;
        lat_lon_of(normalized(a.pos), lat, lon);
        set_text(a.player, wfmt("x %.1f   y %.1f   z %.1f\n", a.pos.x, a.pos.y, a.pos.z) +
                               wfmt("lat %.3f   lon %.3f   r %.1f", lat, lon, length(a.pos)));
    } else {
        set_text(a.player, T("unknown", "неизвестно"));
    }

    update_metrics(a);
    refresh_list_texts(a);
    if ((a.sort_column == COL_DIST || a.sort_column == COL_DIR) && GetTickCount64() - a.last_sort > 3000) {
        sort_list(a);
    }
    const int sel = selected_place(a);
    set_text(a.selinfo, sel >= 0 ? place_details(a, sel) : std::wstring());
}

void warn(App& a, const std::wstring& text) {
    MessageBoxW(a.hwnd, text.c_str(), L"MoonSweepers Trainer", MB_ICONWARNING);
}

void mark_here(App& a) {
    if (!a.game || !a.have_player || length(a.pos) < 1.0f) {
        warn(a, T("Player position is unknown. Load into the game first.", "Позиция игрока неизвестна. Загрузись в игру."));
        return;
    }
    std::string name = narrow(get_text(a.markname));
    if (name.empty()) {
        name = "Mark " + std::to_string(a.storage.marks.size() + 1);
    }
    for (const ms::Mark& m : a.storage.marks) {
        if (m.name == name) {
            warn(a, T("A mark with this name already exists.", "Точка с таким названием уже есть."));
            return;
        }
    }
    a.storage.marks.push_back({name, normalized(a.pos)});
    a.storage.save();
    SetWindowTextW(a.markname, L"");
    rebuild_places(a);
    rebuild_list(a);
}

void delete_mark(App& a) {
    const int sel = selected_place(a);
    if (sel < 0 || a.places[static_cast<size_t>(sel)].kind != Kind::Mark) {
        warn(a, T("Select one of your own marks (type \"Mine\") first.", "Сначала выбери одну из своих точек (тип \"Моя\")."));
        return;
    }
    const std::string name = a.places[static_cast<size_t>(sel)].name;
    a.storage.marks.erase(std::remove_if(a.storage.marks.begin(), a.storage.marks.end(),
                                         [&](const ms::Mark& m) { return m.name == name; }),
                          a.storage.marks.end());
    a.storage.visited.erase("mark:" + name);
    a.storage.save();
    if (a.has_target && a.target.key == "mark:" + name) {
        a.has_target = false;
    }
    rebuild_places(a);
    rebuild_list(a);
}

void toggle_visited(App& a) {
    const int sel = selected_place(a);
    if (sel < 0) {
        return;
    }
    const Place& p = a.places[static_cast<size_t>(sel)];
    if (game_raised(a, p)) {
        warn(a, T("This place is already raised in the game; the game's own mark cannot be removed here.",
                  "Это место уже поднято в игре; отметку игры здесь снять нельзя."));
        return;
    }
    if (!a.storage.visited.erase(p.key)) {
        a.storage.visited.insert(p.key);
    }
    a.storage.save();
    refresh_list_texts(a);
}

void on_command(App& a, int id, int code) {
    switch (id) {
        case IDC_RESCAN:
            if (code == BN_CLICKED) start_attach(a);
            break;
        case IDC_SET: {
            if (kNavigatorOnly || !a.game) break;
            ms::Decimal v;
            if (!parse_edit(a.amount, v)) {
                warn(a, T("Enter a non-negative number, e.g. 10000", "Введи число, например 10000"));
                break;
            }
            if (!a.game->write_balance(v)) {
                status(a, T("Write failed: rescan the game.", "Не удалось записать: найди игру заново."));
            }
            {
                std::lock_guard<std::mutex> lock(a.freezer.mutex);
                a.freezer.value = v;
            }
            break;
        }
        case IDC_FREEZE: {
            if (kNavigatorOnly) break;
            const bool on = SendMessageW(a.freeze, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (on) {
                ms::Decimal v;
                if (!a.game || !parse_edit(a.amount, v)) {
                    SendMessageW(a.freeze, BM_SETCHECK, BST_UNCHECKED, 0);
                    warn(a, T("Enter a valid amount first.", "Сначала введи сумму."));
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(a.freezer.mutex);
                    a.freezer.value = v;
                }
            }
            set_freeze(a, on);
            break;
        }
        case IDC_FREETP: {
            if (kNavigatorOnly) break;
            if (!a.game) {
                SendMessageW(a.freetp, BM_SETCHECK, BST_UNCHECKED, 0);
                break;
            }
            const bool on = SendMessageW(a.freetp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            const ms::Decimal zero;
            if (!a.game->write_setting(ms::Setting::TeleportPrice, on ? zero : a.orig_tp)) {
                status(a, T("Could not change the teleport price.", "Не удалось изменить цену телепорта."));
            }
            break;
        }
        case IDC_APPLY: {
            if (kNavigatorOnly || !a.game) break;
            ms::Decimal fr, sr;
            if (!parse_edit(a.flagrew, fr) || !parse_edit(a.siterew, sr)) {
                warn(a, T("Enter valid reward amounts.", "Введи корректные суммы наград."));
                break;
            }
            a.game->write_setting(ms::Setting::FlagReward, fr);
            a.game->write_setting(ms::Setting::SiteReward, sr);
            a.editing_rewards = false;
            break;
        }
        case IDC_RESTORE:
            if (kNavigatorOnly || !a.game || !a.have_originals) break;
            a.game->write_setting(ms::Setting::FlagReward, a.orig_flag);
            a.game->write_setting(ms::Setting::SiteReward, a.orig_site);
            a.game->write_setting(ms::Setting::TeleportPrice, a.orig_tp);
            SendMessageW(a.freetp, BM_SETCHECK, BST_UNCHECKED, 0);
            a.editing_rewards = false;
            break;
        case IDC_FILTER:
            if (code == CBN_SELCHANGE) rebuild_list(a);
            break;
        case IDC_SEARCH:
            if (code == EN_CHANGE) rebuild_list(a);
            break;
        case IDC_FLAGREW:
        case IDC_SITEREW:
            if (!kNavigatorOnly && code == EN_CHANGE && GetFocus() == (id == IDC_FLAGREW ? a.flagrew : a.siterew)) {
                a.editing_rewards = true;
            }
            break;
        case IDC_COPY: {
            const int sel = selected_place(a);
            if (sel >= 0) copy_to_clipboard(a.hwnd, place_details(a, sel));
            break;
        }
        case IDC_GUIDE: {
            const int sel = selected_place(a);
            if (sel >= 0) {
                open_compass(a, a.places[static_cast<size_t>(sel)]);
            } else {
                warn(a, T("Select a place in the list first.", "Сначала выбери место в списке."));
            }
            break;
        }
        case IDC_VISITED:
            toggle_visited(a);
            break;
        case IDC_MARKHERE:
            mark_here(a);
            break;
        case IDC_MARKDEL:
            delete_mark(a);
            break;
        default: break;
    }
}

LRESULT on_notify(App& a, NMHDR* nm) {
    if (nm->idFrom != IDC_LIST) {
        return 0;
    }
    if (nm->code == LVN_COLUMNCLICK) {
        const int col = reinterpret_cast<NMLISTVIEW*>(nm)->iSubItem;
        if (col == a.sort_column) {
            a.sort_ascending = !a.sort_ascending;
        } else {
            a.sort_column = col;
            a.sort_ascending = true;
        }
        sort_list(a);
    } else if (nm->code == NM_DBLCLK) {
        const int sel = selected_place(a);
        if (sel >= 0) {
            open_compass(a, a.places[static_cast<size_t>(sel)]);  // double-click: guide me there
        }
    }
    return 0;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            a = static_cast<App*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
            a->hwnd = hwnd;
            g_app = a;
            HDC dc = GetDC(nullptr);
            a->dpi = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(nullptr, dc);
            NONCLIENTMETRICSW ncm{};
            ncm.cbSize = sizeof(ncm);
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            a->font = CreateFontIndirectW(&ncm.lfMessageFont);
            LOGFONTW big = ncm.lfMessageFont;
            big.lfHeight = -a->S(30);
            big.lfWeight = FW_BOLD;
            a->compass_big = CreateFontIndirectW(&big);
            LOGFONTW small = ncm.lfMessageFont;
            small.lfHeight = -a->S(16);
            small.lfWeight = FW_SEMIBOLD;
            a->compass_small = CreateFontIndirectW(&small);
            a->storage.load();
            create_controls(*a);
            layout(*a);
            if (!kNavigatorOnly) {
                a->freezer.start();
            }
            rebuild_places(*a);  // your own marks show up even before the game is found
            rebuild_list(*a);
            SetTimer(hwnd, kTimer, 400, nullptr);
            start_attach(*a);
            return 0;
        }
        case WM_SIZE:
            if (a) layout(*a);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const int dpi = a ? a->dpi : 96;
            mmi->ptMinTrackSize.x = MulDiv(980, dpi, 96);
            mmi->ptMinTrackSize.y = MulDiv(640, dpi, 96);
            return 0;
        }
        case WM_TIMER:
            if (a && wp == kTimer) on_timer(*a);
            return 0;
        case WM_COMMAND:
            if (a) on_command(*a, LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_NOTIFY:
            return a ? on_notify(*a, reinterpret_cast<NMHDR*>(lp)) : 0;
        case WM_ATTACHED: {
            std::unique_ptr<std::shared_ptr<ms::Game>> payload(reinterpret_cast<std::shared_ptr<ms::Game>*>(lp));
            if (a) on_attached(*a, *payload);
            return 0;
        }
        case WM_ATTACH_FAILED: {
            std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lp));
            if (a) {
                a->attaching = false;
                EnableWindow(a->rescan, TRUE);
                status(*a, T("Not connected: ", "Нет подключения: ") + widen(*payload));
            }
            return 0;
        }
        case WM_DESTROY:
            if (a) {
                KillTimer(hwnd, kTimer);
                if (a->compass) DestroyWindow(a->compass);
                if (!kNavigatorOnly) {
                    a->freezer.stop();
                    restore_game_settings(*a);  // do not leave the game with changed prices behind
                }
                if (a->font) DeleteObject(a->font);
                if (a->compass_big) DeleteObject(a->compass_big);
                if (a->compass_small) DeleteObject(a->compass_small);
            }
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_russian = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MoonSweepersTrainerWindow";
    RegisterClassExW(&wc);

    WNDCLASSEXW cc{};
    cc.cbSize = sizeof(cc);
    cc.lpfnWndProc = compass_proc;
    cc.hInstance = instance;
    cc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cc.lpszClassName = kCompassClass;
    RegisterClassExW(&cc);

    App app;
    const std::wstring title = std::wstring(kNavigatorOnly ? L"MoonSweepers Navigator " : L"MoonSweepers Trainer ") +
                               widen(TRAINER_VERSION);
    const HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                      CW_USEDEFAULT, 1120, 740, nullptr, nullptr, instance, &app);
    if (hwnd == nullptr) {
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
