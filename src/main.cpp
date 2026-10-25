#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "decimal.hpp"
#include "process.hpp"
#include "wallet.hpp"

#ifndef TRAINER_VERSION
#define TRAINER_VERSION "dev"
#endif

namespace {

constexpr wchar_t kExeName[] = L"MoonSweepers.exe";

std::atomic<bool> g_freezing{false};
std::atomic<bool> g_stop{false};

BOOL WINAPI on_ctrl(DWORD type) {
    // While freezing, Ctrl+C only ends the freeze; elsewhere it exits as usual.
    if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) && g_freezing) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

struct Options {
    bool help = false;
    bool version = false;
    bool list = false;
    int wallet = -1;
    std::string set;
    std::string freeze;
    std::string match;

    bool interactive() const { return !list && set.empty() && freeze.empty(); }
};

void print_usage() {
    std::cout <<
        "MoonSweepers trainer " TRAINER_VERSION "  (single-player use only)\n"
        "\n"
        "Usage: moonsweepers-trainer [options]\n"
        "  (no options)        interactive menu\n"
        "  --list              show the wallets found in the running game\n"
        "  --set AMOUNT        set the balance, e.g. --set 10000\n"
        "  --freeze AMOUNT     keep the balance at AMOUNT until Ctrl+C\n"
        "  --wallet N          use wallet number N from --list\n"
        "  --match AMOUNT      pick the wallet whose balance shows as AMOUNT on screen\n"
        "  --version, --help\n"
        "\n"
        "Start the game first. If the process cannot be opened, run as administrator.\n";
}

bool parse_args(int argc, char** argv, Options& opt) {
    auto value = [&](int& i, std::string& out) {
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << argv[i] << "\n";
            return false;
        }
        out = argv[++i];
        return true;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string num;
        if (arg == "--help" || arg == "-h") {
            opt.help = true;
        } else if (arg == "--version") {
            opt.version = true;
        } else if (arg == "--list") {
            opt.list = true;
        } else if (arg == "--set") {
            if (!value(i, opt.set)) return false;
        } else if (arg == "--freeze") {
            if (!value(i, opt.freeze)) return false;
        } else if (arg == "--match") {
            if (!value(i, opt.match)) return false;
        } else if (arg == "--wallet") {
            if (!value(i, num)) return false;
            char* end = nullptr;
            const long n = std::strtol(num.c_str(), &end, 10);
            if (end == num.c_str() || *end != '\0' || n < 0 || n > 1000000) {
                std::cerr << "Invalid wallet number: " << num << "\n";
                return false;
            }
            opt.wallet = static_cast<int>(n);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }
    if (!opt.set.empty() && !opt.freeze.empty()) {
        std::cerr << "Use either --set or --freeze, not both.\n";
        return false;
    }
    return true;
}

bool read_line(const char* prompt, std::string& out) {
    std::cout << prompt << std::flush;
    return static_cast<bool>(std::getline(std::cin, out));
}

std::string balance_text(const ms::Process& process, const ms::Wallet& wallet) {
    ms::Decimal d;
    return ms::read_balance(process, wallet, d) ? ms::to_string(d) : std::string("<unavailable>");
}

void print_wallets(const ms::Process& process, const std::vector<ms::Wallet>& wallets) {
    for (size_t i = 0; i < wallets.size(); ++i) {
        std::cout << "  [" << i << "] object 0x" << std::hex << wallets[i].object << std::dec
                  << "  balance " << balance_text(process, wallets[i]) << "\n";
    }
}

// Returns the index of the chosen wallet, or -1 (message already printed).
int choose_wallet(const ms::Process& process, const std::vector<ms::Wallet>& wallets,
                  const Options& opt) {
    if (!opt.match.empty()) {
        ms::Decimal typed;
        if (!ms::parse_decimal(opt.match, typed)) {
            std::cerr << "Invalid amount for --match: " << opt.match << "\n";
            return -1;
        }
        int found = -1;
        int count = 0;
        for (size_t i = 0; i < wallets.size(); ++i) {
            ms::Decimal d;
            if (ms::read_balance(process, wallets[i], d) && ms::display_matches(d, typed)) {
                found = static_cast<int>(i);
                ++count;
            }
        }
        if (count == 1) {
            return found;
        }
        std::cerr << (count == 0 ? "No wallet matches " : "Several wallets match ") << opt.match
                  << ". Wallets:\n";
        print_wallets(process, wallets);
        return -1;
    }
    if (opt.wallet >= 0) {
        if (static_cast<size_t>(opt.wallet) >= wallets.size()) {
            std::cerr << "No wallet number " << opt.wallet << ". Wallets:\n";
            print_wallets(process, wallets);
            return -1;
        }
        return opt.wallet;
    }
    if (wallets.size() == 1) {
        return 0;
    }
    std::cout << "Several wallets found:\n";
    print_wallets(process, wallets);
    if (!opt.interactive()) {
        std::cerr << "Pick one with --wallet N or --match AMOUNT.\n";
        return -1;
    }
    std::cout << "Choose the one whose balance equals the amount shown in the game.\n";
    for (;;) {
        std::string line;
        if (!read_line("Wallet number: ", line)) {
            return -1;
        }
        char* end = nullptr;
        const long n = std::strtol(line.c_str(), &end, 10);
        if (end != line.c_str() && n >= 0 && static_cast<size_t>(n) < wallets.size()) {
            return static_cast<int>(n);
        }
        std::cout << "Enter a number from the list.\n";
    }
}

bool set_balance(const ms::Process& process, const ms::Wallet& wallet, const std::string& text) {
    ms::Decimal value;
    if (!ms::parse_decimal(text, value)) {
        std::cerr << "Invalid amount: " << text << "\n";
        return false;
    }
    if (!ms::write_balance(process, wallet, value)) {
        std::cerr << "Write failed: the wallet moved or the game closed. Rescan and try again.\n";
        return false;
    }
    std::cout << "Balance is now " << balance_text(process, wallet) << "\n";
    return true;
}

bool freeze_balance(const ms::Process& process, const ms::Wallet& wallet, const std::string& text) {
    ms::Decimal value;
    if (!ms::parse_decimal(text, value)) {
        std::cerr << "Invalid amount: " << text << "\n";
        return false;
    }
    std::cout << "Holding the balance at " << ms::to_string(value) << ". Press Ctrl+C to stop.\n";
    g_stop = false;
    g_freezing = true;
    bool ok = true;
    while (!g_stop && process.alive()) {
        if (!ms::write_balance(process, wallet, value)) {
            std::cerr << "Lost the wallet (the game moved it or closed).\n";
            ok = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    g_freezing = false;
    std::cout << "Stopped.\n";
    return ok;
}

bool locate(const ms::Process& process, std::vector<ms::Wallet>& wallets) {
    std::cout << "Searching for wallets (a few seconds)...\n";
    wallets = ms::find_wallets(process);
    if (wallets.empty()) {
        std::cerr << "No wallet found. Load into a game session (not just the main menu) and retry.\n";
        return false;
    }
    return true;
}

int run_menu(const ms::Process& process, std::vector<ms::Wallet>& wallets, int& index,
             const Options& opt) {
    for (;;) {
        std::cout << "\nWallet [" << index << "] balance: "
                  << balance_text(process, wallets[static_cast<size_t>(index)]) << "\n"
                  << "  1) set balance\n  2) freeze balance\n  3) rescan wallets\n  0) quit\n";
        std::string choice;
        if (!read_line("> ", choice) || choice == "0") {
            return 0;
        }
        if (choice == "1" || choice == "2") {
            std::string amount;
            if (!read_line("Amount: ", amount)) {
                return 0;
            }
            const ms::Wallet& w = wallets[static_cast<size_t>(index)];
            if (choice == "1") {
                set_balance(process, w, amount);
            } else {
                freeze_balance(process, w, amount);
            }
        } else if (choice == "3") {
            if (!locate(process, wallets)) {
                return 1;
            }
            Options pick = opt;  // keep --wallet / --match from the command line
            index = choose_wallet(process, wallets, pick);
            if (index < 0) {
                return 1;
            }
        }
    }
}

int run(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        print_usage();
        return 2;
    }
    if (opt.help) {
        print_usage();
        return 0;
    }
    if (opt.version) {
        std::cout << TRAINER_VERSION << "\n";
        return 0;
    }

    const DWORD pid = ms::Process::find_pid(kExeName);
    if (pid == 0) {
        std::cerr << "MoonSweepers.exe is not running. Start the game first.\n";
        return 1;
    }
    ms::Process process;
    if (!process.open(pid)) {
        std::cerr << "Cannot open the game process (error " << GetLastError()
                  << "). Try running as administrator.\n";
        return 1;
    }

    std::vector<ms::Wallet> wallets;
    if (!locate(process, wallets)) {
        return 1;
    }
    if (opt.list) {
        print_wallets(process, wallets);
        return 0;
    }
    int index = choose_wallet(process, wallets, opt);
    if (index < 0) {
        return 1;
    }
    const ms::Wallet& wallet = wallets[static_cast<size_t>(index)];

    if (!opt.set.empty()) {
        return set_balance(process, wallet, opt.set) ? 0 : 1;
    }
    if (!opt.freeze.empty()) {
        return freeze_balance(process, wallet, opt.freeze) ? 0 : 1;
    }
    return run_menu(process, wallets, index, opt);
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    int code = 1;
    try {
        code = run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
    if (argc == 1) {  // started by double click: keep the window open
        std::string ignored;
        read_line("\nPress Enter to exit...", ignored);
    }
    return code;
}
