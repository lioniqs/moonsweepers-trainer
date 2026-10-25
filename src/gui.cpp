// Win32 window for the trainer: wallet, free teleport / rewards, and a navigator to every
// country flag and landing site. No third-party libraries.

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

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifndef TRAINER_VERSION
#define TRAINER_VERSION "dev"
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

// ------------------------------------------------------------------- the app

constexpr UINT WM_ATTACHED = WM_APP + 1;
constexpr UINT WM_ATTACH_FAILED = WM_APP + 2;
constexpr UINT_PTR kTimer = 1;

enum Id {
    IDC_STATUS = 100, IDC_RESCAN, IDC_GRP_WALLET, IDC_BALANCE, IDC_LBL_AMOUNT, IDC_AMOUNT, IDC_SET,
    IDC_FREEZE, IDC_GRP_GAME, IDC_FREETP, IDC_TPINFO, IDC_LBL_FLAG, IDC_FLAGREW, IDC_LBL_SITE,
    IDC_SITEREW, IDC_APPLY, IDC_RESTORE, IDC_GRP_PLAYER, IDC_PLAYER, IDC_WARN, IDC_FILTER,
    IDC_SEARCH, IDC_LIST, IDC_SELINFO, IDC_COPY
};

enum Column { COL_NUM, COL_TYPE, COL_NAME, COL_LAT, COL_LON, COL_DIST, COL_DIR, COL_COUNT };

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

struct Metrics {
    double distance = -1;  // metres along the surface, -1 unknown
    double relative = 0;   // degrees, positive = to the right of the player's heading
    bool has_direction = false;
};

struct App {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    int dpi = 96;

    HWND status = nullptr, rescan = nullptr, grp_wallet = nullptr, balance = nullptr, lbl_amount = nullptr,
         amount = nullptr, set_btn = nullptr, freeze = nullptr, grp_game = nullptr, freetp = nullptr,
         tpinfo = nullptr, lbl_flag = nullptr, flagrew = nullptr, lbl_site = nullptr, siterew = nullptr,
         apply = nullptr, restore = nullptr, grp_player = nullptr, player = nullptr, warn = nullptr,
         filter = nullptr, search = nullptr, list = nullptr, selinfo = nullptr, copy = nullptr;

    std::shared_ptr<ms::Game> game;
    bool attaching = false;
    Freezer freezer;

    bool have_originals = false;
    ms::Decimal orig_tp, orig_flag, orig_site;
    bool tp_modified = false;

    Vec3 pos, heading;
    bool have_player = false;
    std::vector<Metrics> metrics;  // per landmark (same order as game->landmarks())

    int sort_column = COL_DIST;
    bool sort_ascending = true;
    ULONGLONG last_sort = 0;
    bool editing_rewards = false;

    int S(int v) const { return MulDiv(v, dpi, 96); }
};

App* g_app = nullptr;  // only for the list-view sort callback

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

void place(HWND h, int x, int y, int w, int hgt) { MoveWindow(h, x, y, w, hgt, TRUE); }

void layout(App& a) {
    RECT rc;
    GetClientRect(a.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = a.S(10);
    const int left_w = a.S(350);
    const int row = a.S(24);

    place(a.status, m, a.S(12), W - a.S(150), a.S(20));
    place(a.rescan, W - a.S(130) - m + a.S(10), a.S(8), a.S(120), a.S(28));

    int y = a.S(44);
    // wallet
    place(a.grp_wallet, m, y, left_w, a.S(150));
    place(a.balance, m + a.S(12), y + a.S(22), left_w - a.S(24), a.S(20));
    place(a.lbl_amount, m + a.S(12), y + a.S(52), left_w - a.S(24), a.S(18));
    place(a.amount, m + a.S(12), y + a.S(72), a.S(170), row);
    place(a.set_btn, m + a.S(192), y + a.S(71), left_w - a.S(204), a.S(26));
    place(a.freeze, m + a.S(12), y + a.S(110), left_w - a.S(24), a.S(22));
    y += a.S(160);
    // game settings
    place(a.grp_game, m, y, left_w, a.S(200));
    place(a.freetp, m + a.S(12), y + a.S(22), left_w - a.S(24), a.S(22));
    place(a.tpinfo, m + a.S(30), y + a.S(46), left_w - a.S(42), a.S(20));
    place(a.lbl_flag, m + a.S(12), y + a.S(80), a.S(150), a.S(20));
    place(a.flagrew, m + a.S(170), y + a.S(76), left_w - a.S(182), row);
    place(a.lbl_site, m + a.S(12), y + a.S(112), a.S(150), a.S(20));
    place(a.siterew, m + a.S(170), y + a.S(108), left_w - a.S(182), row);
    place(a.apply, m + a.S(12), y + a.S(150), (left_w - a.S(36)) / 2, a.S(28));
    place(a.restore, m + a.S(24) + (left_w - a.S(36)) / 2, y + a.S(150), (left_w - a.S(36)) / 2, a.S(28));
    y += a.S(210);
    // player
    place(a.grp_player, m, y, left_w, a.S(86));
    place(a.player, m + a.S(12), y + a.S(20), left_w - a.S(24), a.S(60));
    y += a.S(94);
    place(a.warn, m, y, left_w, H - y - m);

    // right side: navigator
    const int rx = m + left_w + m;
    const int rw = W - rx - m;
    place(a.filter, rx, a.S(44), a.S(150), a.S(200));
    place(a.search, rx + a.S(160), a.S(44), rw - a.S(160), row);
    place(a.list, rx, a.S(76), rw, H - a.S(76) - a.S(70));
    place(a.selinfo, rx, H - a.S(60), rw - a.S(150), a.S(48));
    place(a.copy, rx + rw - a.S(140), H - a.S(58), a.S(140), a.S(28));
}

void add_columns(App& a) {
    const struct {
        const char* en;
        const char* ru;
        int width;
        int fmt;
    } cols[COL_COUNT] = {
        {"#", "#", 44, LVCFMT_RIGHT},          {"Type", "Тип", 62, LVCFMT_LEFT},
        {"Name", "Название", 200, LVCFMT_LEFT}, {"Lat", "Шир.", 70, LVCFMT_RIGHT},
        {"Lon", "Долг.", 74, LVCFMT_RIGHT},     {"Distance", "Дистанция", 90, LVCFMT_RIGHT},
        {"Direction", "Направление", 100, LVCFMT_LEFT},
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

    a.grp_player = make(a, L"BUTTON", T("Player", "Игрок"), BS_GROUPBOX, IDC_GRP_PLAYER);
    a.player = make(a, L"STATIC", L"", SS_LEFT, IDC_PLAYER);
    a.warn = make(a, L"STATIC",
                  T("This game is co-op. Changes are meant for your own session only; do not use them in "
                    "other players' crews.",
                    "Игра кооперативная. Используй изменения только в своей сессии, не в чужих экипажах."),
                  SS_LEFT, IDC_WARN);

    a.filter = make(a, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_FILTER);
    SendMessageW(a.filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("All", "Все").c_str()));
    SendMessageW(a.filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Flags", "Флаги").c_str()));
    SendMessageW(a.filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Sites", "Метки").c_str()));
    SendMessageW(a.filter, CB_SETCURSEL, 0, 0);
    a.search = make(a, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SEARCH, WS_EX_CLIENTEDGE);
    SendMessageW(a.search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(T("Search by name...", "Поиск по названию...").c_str()));

    a.list = make(a, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_LIST,
                  WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(a.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    add_columns(a);
    a.selinfo = make(a, L"STATIC", L"", SS_LEFT, IDC_SELINFO);
    a.copy = make(a, L"BUTTON", T("Copy coordinates", "Копировать координаты"), BS_PUSHBUTTON | WS_TABSTOP, IDC_COPY);

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

void rebuild_list(App& a) {
    ListView_DeleteAllItems(a.list);
    if (!a.game) {
        return;
    }
    const int mode = filter_mode(a);
    const std::wstring needle = get_text(a.search);
    const auto& lms = a.game->landmarks();

    SendMessageW(a.list, WM_SETREDRAW, FALSE, 0);
    int row = 0;
    for (size_t i = 0; i < lms.size(); ++i) {
        const ms::Landmark& lm = lms[i];
        if ((mode == 1 && !lm.flag) || (mode == 2 && lm.flag)) {
            continue;
        }
        const std::wstring name = widen(lm.name);
        if (!contains_nocase(name, needle)) {
            continue;
        }
        std::wstring num = std::to_wstring(lm.index);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = &num[0];
        item.lParam = static_cast<LPARAM>(i);
        SendMessageW(a.list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));

        auto sub = [&](int col, std::wstring text) {
            ListView_SetItemText(a.list, row, col, &text[0]);
        };
        sub(COL_TYPE, lm.flag ? T("Flag", "Флаг") : T("Site", "Метка"));
        sub(COL_NAME, name);
        sub(COL_LAT, wfmt("%.3f", lm.lat));
        sub(COL_LON, wfmt("%.3f", lm.lon));
        ++row;
    }
    SendMessageW(a.list, WM_SETREDRAW, TRUE, 0);
}

int CALLBACK compare_rows(LPARAM l1, LPARAM l2, LPARAM) {
    App& a = *g_app;
    if (!a.game) {
        return 0;
    }
    const auto& lms = a.game->landmarks();
    const size_t i = static_cast<size_t>(l1), j = static_cast<size_t>(l2);
    int r = 0;
    switch (a.sort_column) {
        case COL_NUM: r = lms[i].index - lms[j].index; break;
        case COL_TYPE: r = static_cast<int>(lms[i].flag) - static_cast<int>(lms[j].flag); break;
        case COL_NAME: r = lms[i].name.compare(lms[j].name); break;
        case COL_LAT: r = lms[i].lat < lms[j].lat ? -1 : (lms[i].lat > lms[j].lat ? 1 : 0); break;
        case COL_LON: r = lms[i].lon < lms[j].lon ? -1 : (lms[i].lon > lms[j].lon ? 1 : 0); break;
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
    if (!a.game) {
        return;
    }
    const auto& lms = a.game->landmarks();
    a.metrics.assign(lms.size(), Metrics{});
    if (!a.have_player) {
        return;
    }
    const float r = length(a.pos);
    if (r < 1.0f) {
        return;
    }
    const Vec3 up = normalized(a.pos);
    Vec3 fwd = a.heading - up * dot(a.heading, up);
    const bool has_heading = length(fwd) > 1e-4f;
    fwd = normalized(fwd);
    const Vec3 right = cross(up, fwd);  // Unity: cross(up, forward) points to the player's right

    for (size_t i = 0; i < lms.size(); ++i) {
        const Vec3 d = lms[i].dir;
        const float c = std::max(-1.0f, std::min(1.0f, dot(up, d)));
        Metrics& m = a.metrics[i];
        m.distance = static_cast<double>(r) * std::acos(c);
        const Vec3 t = normalized(d - up * dot(d, up));
        if (has_heading && length(t) > 0.5f) {
            m.relative = std::atan2(static_cast<double>(dot(t, right)), static_cast<double>(dot(t, fwd))) *
                         180.0 / kPi;
            m.has_direction = true;
        }
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
        ListView_SetItemText(a.list, row, COL_DIST, &d[0]);
        ListView_SetItemText(a.list, row, COL_DIR, &dir[0]);
    }
}

int selected_landmark(App& a) {
    const int row = ListView_GetNextItem(a.list, -1, LVNI_SELECTED);
    if (row < 0) {
        return -1;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    return ListView_GetItem(a.list, &item) ? static_cast<int>(item.lParam) : -1;
}

std::wstring landmark_details(App& a, int idx) {
    const ms::Landmark& lm = a.game->landmarks()[static_cast<size_t>(idx)];
    // Approximate world position: direction * the player's current distance from the Moon's centre.
    const float r = a.have_player ? length(a.pos) : 2002.4f;
    return wfmt("%s (%s)\nlat %.4f, lon %.4f\nworld ~ (%.1f, %.1f, %.1f)", lm.name.c_str(),
                lm.flag ? "flag" : "site", lm.lat, lm.lon, lm.dir.x * r, lm.dir.y * r, lm.dir.z * r);
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
    {
        std::lock_guard<std::mutex> lock(a.freezer.mutex);
        a.freezer.game = game;
    }
    if (!a.have_originals) {
        a.have_originals = game->read_setting(ms::Setting::TeleportPrice, a.orig_tp) &&
                           game->read_setting(ms::Setting::FlagReward, a.orig_flag) &&
                           game->read_setting(ms::Setting::SiteReward, a.orig_site);
    }
    status(a, T("Connected. Landmarks loaded: ", "Подключено. Загружено меток: ") +
                  std::to_wstring(game->landmarks().size()));
    rebuild_list(a);
    a.last_sort = 0;
}

void restore_game_settings(App& a) {
    if (a.game && a.have_originals && a.game->alive()) {
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
        {
            std::lock_guard<std::mutex> lock(a.freezer.mutex);
            a.freezer.game.reset();
        }
        status(a, T("The game was closed. Start it and press \"Rescan game\".",
                    "Игра закрыта. Запусти её и нажми \"Найти игру заново\"."));
        return;
    }

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

    a.have_player = a.game->player(a.pos, a.heading);
    if (a.have_player) {
        const float r = length(a.pos);
        const double lat = r > 1 ? std::asin(static_cast<double>(a.pos.y / r)) * 180.0 / kPi : 0.0;
        const double lon = std::atan2(static_cast<double>(a.pos.x), -static_cast<double>(a.pos.z)) * 180.0 / kPi;
        set_text(a.player, wfmt("x %.1f   y %.1f   z %.1f\n", a.pos.x, a.pos.y, a.pos.z) +
                               wfmt("lat %.3f   lon %.3f   r %.1f", lat, lon, r));
    } else {
        set_text(a.player, T("unknown", "неизвестно"));
    }

    update_metrics(a);
    refresh_list_texts(a);
    if ((a.sort_column == COL_DIST || a.sort_column == COL_DIR) && GetTickCount64() - a.last_sort > 3000) {
        sort_list(a);
    }
    const int sel = selected_landmark(a);
    set_text(a.selinfo, sel >= 0 ? landmark_details(a, sel) : std::wstring());
}

void on_command(App& a, int id, int code) {
    switch (id) {
        case IDC_RESCAN:
            if (code == BN_CLICKED) start_attach(a);
            break;
        case IDC_SET: {
            if (!a.game) break;
            ms::Decimal v;
            if (!parse_edit(a.amount, v)) {
                MessageBoxW(a.hwnd, T("Enter a non-negative number, e.g. 10000", "Введи число, например 10000").c_str(),
                            L"MoonSweepers Trainer", MB_ICONWARNING);
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
            const bool on = SendMessageW(a.freeze, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (on) {
                ms::Decimal v;
                if (!a.game || !parse_edit(a.amount, v)) {
                    SendMessageW(a.freeze, BM_SETCHECK, BST_UNCHECKED, 0);
                    MessageBoxW(a.hwnd, T("Enter a valid amount first.", "Сначала введи сумму.").c_str(),
                                L"MoonSweepers Trainer", MB_ICONWARNING);
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
            if (!a.game) break;
            ms::Decimal fr, sr;
            if (!parse_edit(a.flagrew, fr) || !parse_edit(a.siterew, sr)) {
                MessageBoxW(a.hwnd, T("Enter valid reward amounts.", "Введи корректные суммы наград.").c_str(),
                            L"MoonSweepers Trainer", MB_ICONWARNING);
                break;
            }
            a.game->write_setting(ms::Setting::FlagReward, fr);
            a.game->write_setting(ms::Setting::SiteReward, sr);
            a.editing_rewards = false;
            break;
        }
        case IDC_RESTORE:
            if (!a.game || !a.have_originals) break;
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
            if (code == EN_CHANGE && GetFocus() == (id == IDC_FLAGREW ? a.flagrew : a.siterew)) {
                a.editing_rewards = true;
            }
            break;
        case IDC_COPY: {
            const int sel = selected_landmark(a);
            if (a.game && sel >= 0) copy_to_clipboard(a.hwnd, landmark_details(a, sel));
            break;
        }
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
        const int sel = selected_landmark(a);
        if (a.game && sel >= 0) {
            copy_to_clipboard(a.hwnd, landmark_details(a, sel));
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
            create_controls(*a);
            layout(*a);
            a->freezer.start();
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
            mmi->ptMinTrackSize.x = MulDiv(940, dpi, 96);
            mmi->ptMinTrackSize.y = MulDiv(620, dpi, 96);
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
                a->freezer.stop();
                restore_game_settings(*a);  // do not leave the game with changed prices behind
                if (a->font) DeleteObject(a->font);
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

    App app;
    const std::wstring title = L"MoonSweepers Trainer " + widen(TRAINER_VERSION);
    const HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                      CW_USEDEFAULT, 1060, 700, nullptr, nullptr, instance, &app);
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
