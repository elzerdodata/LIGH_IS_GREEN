// Light is Green v1.0 for Nintendo Switch.
//
// SDL2 frontend: splash -> device-code sign-in -> game library grid with box
// art and search -> native WebRTC streaming (see src/switch/stream/).

#include <SDL2/SDL.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>  // rmdir: removing an account's directory

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../vendor/json.hpp"
#include "../core/auth.hpp"
#include "../core/catalog.hpp"
#include "covers.hpp"
#include "gfx.hpp"

#ifdef GNX_NATIVE_STREAM
#include "stream/engine.hpp"
#endif

using nlohmann::json;
using namespace gnx;

#ifndef GNX_VERSION
#define GNX_VERSION "dev"
#endif

namespace {

#ifdef __SWITCH__
constexpr const char* kDataDir = "sdmc:/switch/light-is-green";
constexpr const char* kLegacyDataDir = "sdmc:/switch/green-nx";
#else
const std::string kDataDirStr =
    std::string(getenv("HOME")) + "/.light-is-green";
const std::string kLegacyDataDirStr =
    std::string(getenv("HOME")) + "/.green-nx";
const char* kDataDir = kDataDirStr.c_str();
const char* kLegacyDataDir = kLegacyDataDirStr.c_str();
#endif

std::string data_path(const char* leaf) {
    return std::string(kDataDir) + "/" + leaf;
}

// ---- Accounts -------------------------------------------------------------
// More than one person can use the same console, so each signed-in account
// owns a directory under users/ holding its tokens and its cached library:
//   sdmc:/switch/light-is-green/users/<id>/{tokens,games,consoles,favorites,history}.json
// Device-wide state (settings, box art, logs) stays at the root, so switching
// accounts keeps the console's own preferences and never re-downloads covers.
// users.json is the registry: the known accounts plus which one is active.
struct Account {
    std::string id;        // directory name ("u1", "u2", ...)
    std::string gamertag;  // label for the picker; filled in after sign-in
};

std::vector<Account> g_accounts;
std::string g_active_account = "u1";
// Non-empty while the sign-in screen is showing because we switched to an
// account that has no saved login: the account to fall back to if the user
// backs out. Empty means sign-in is the app's own entry point.
std::string g_signin_return_account;

std::string account_dir(const std::string& id) {
    return std::string(kDataDir) + "/users/" + id;
}

// Path to a file owned by the active account (vs. data_path = device-wide).
std::string user_path(const char* leaf) {
    return account_dir(g_active_account) + "/" + leaf;
}

// Files that belong to an account rather than to the console.
const char* const kAccountFiles[] = {"tokens.json", "games.json",
                                     "consoles.json", "favorites.json",
                                     "history.json"};

// v1 owns a new data directory. Import the small account/configuration files
// from the prior directory exactly once, without moving or overwriting anything.
// Cover art deliberately stays behind: it is disposable cache data and copying
// it can make the first v1 launch slow or duplicate a large directory on SD.
const char* const kDeviceFiles[] = {"settings.json", "server-regions.json",
                                    "users.json", "consoles-log.txt"};

bool path_exists(const std::string& path) {
    struct stat status {};
    return stat(path.c_str(), &status) == 0;
}

bool directory_exists(const std::string& path) {
    struct stat status {};
    return stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

bool copy_file_if_missing(const std::string& source,
                          const std::string& destination) {
    if (path_exists(destination)) return true;
    std::ifstream input(source, std::ios::binary);
    if (!input) return false;
    std::ofstream output(destination, std::ios::binary);
    if (!output) return false;
    output << input.rdbuf();
    return static_cast<bool>(output);
}

void migrate_legacy_data_if_needed() {
    mkdir(kDataDir, 0755);
    if (!directory_exists(kLegacyDataDir)) return;

    const std::string old_root = kLegacyDataDir;
    const std::string new_root = kDataDir;
    for (const char* leaf : kDeviceFiles)
        copy_file_if_missing(old_root + "/" + leaf, new_root + "/" + leaf);

    const std::string old_users = old_root + "/users";
    if (!directory_exists(old_users)) return;
    const std::string new_users = new_root + "/users";
    mkdir(new_users.c_str(), 0755);

    DIR* directory = opendir(old_users.c_str());
    if (!directory) return;
    while (dirent* entry = readdir(directory)) {
        const std::string id = entry->d_name;
        if (id == "." || id == "..") continue;
        const std::string old_account = old_users + "/" + id;
        if (!directory_exists(old_account)) continue;
        const std::string new_account = new_users + "/" + id;
        mkdir(new_account.c_str(), 0755);
        for (const char* leaf : kAccountFiles)
            copy_file_if_missing(old_account + "/" + leaf,
                                 new_account + "/" + leaf);
    }
    closedir(directory);
}

void make_account_dir(const std::string& id) {
    mkdir((std::string(kDataDir) + "/users").c_str(), 0755);
    mkdir(account_dir(id).c_str(), 0755);
}

// Has this account ever completed a sign-in (i.e. is there a token store)?
bool account_has_login(const std::string& id) {
    std::ifstream token(account_dir(id) + "/tokens.json");
    return static_cast<bool>(token);
}

bool account_exists(const std::string& id) {
    for (const Account& account : g_accounts)
        if (account.id == id) return true;
    return false;
}

void save_accounts() {
    json users = json::array();
    for (const Account& account : g_accounts)
        users.push_back({{"id", account.id}, {"gamertag", account.gamertag}});
    std::ofstream out(data_path("users.json"), std::ios::trunc);
    out << json{{"active", g_active_account}, {"users", users}}.dump(2);
}

std::string next_account_id() {
    for (int n = 1;; ++n) {
        std::string id = "u" + std::to_string(n);
        bool taken = false;
        for (const Account& account : g_accounts)
            taken = taken || account.id == id;
        if (!taken) return id;
    }
}

// Load the registry. On an install from before accounts existed, adopt the
// root-level files as the first account, so an existing user keeps their
// sign-in, library and favorites without noticing the change.
void load_accounts() {
    std::ifstream in(data_path("users.json"));
    json data = json::parse(in, nullptr, false);
    if (!data.is_discarded() && data.contains("users")) {
        for (const json& entry : data["users"]) {
            Account account;
            account.id = entry.value("id", "");
            account.gamertag = entry.value("gamertag", "");
            if (!account.id.empty()) g_accounts.push_back(std::move(account));
        }
        g_active_account = data.value("active", "");
    }
    if (g_accounts.empty()) {
        g_accounts.push_back({"u1", ""});
        g_active_account = "u1";
        make_account_dir("u1");
        for (const char* leaf : kAccountFiles)
            std::rename(data_path(leaf).c_str(),
                        (account_dir("u1") + "/" + leaf).c_str());
        save_accounts();
    }
    // A registry pointing at a removed account would leave nothing loadable.
    bool active_exists = false;
    for (const Account& account : g_accounts)
        active_exists = active_exists || account.id == g_active_account;
    if (!active_exists) g_active_account = g_accounts.front().id;
    // An account added but never signed into has nothing to load, and landing
    // on the sign-in screen at startup hides the accounts that do work (the
    // picker is two menus deep from there). Start on a signed-in account if
    // the console has one.
    if (!account_has_login(g_active_account)) {
        for (const Account& account : g_accounts) {
            if (account_has_login(account.id)) {
                g_active_account = account.id;
                break;
            }
        }
    }
    make_account_dir(g_active_account);
}

// Drop an account from the console: its files, its directory and its registry
// entry. The caller decides which account becomes active afterwards.
void remove_account(const std::string& id) {
    for (const char* leaf : kAccountFiles)
        std::remove((account_dir(id) + "/" + leaf).c_str());
    rmdir(account_dir(id).c_str());  // empty now; fails harmlessly if not
    g_accounts.erase(std::remove_if(g_accounts.begin(), g_accounts.end(),
                                    [&id](const Account& account) {
                                        return account.id == id;
                                    }),
                     g_accounts.end());
    save_accounts();
}

// Remember the gamertag so the picker can label accounts by name.
void remember_gamertag(const std::string& gamertag) {
    if (gamertag.empty()) return;
    for (Account& account : g_accounts) {
        if (account.id == g_active_account && account.gamertag != gamertag) {
            account.gamertag = gamertag;
            save_accounts();
        }
    }
}

// ---- Switch joystick button indices (libnx SDL2 port) ---------------------
enum JoyButton {
    kBtnA = 0, kBtnB = 1, kBtnX = 2, kBtnY = 3,
    kBtnL = 6, kBtnR = 7, kBtnZL = 8, kBtnZR = 9,
    kBtnPlus = 10, kBtnMinus = 11,
    kBtnLeft = 12, kBtnUp = 13, kBtnRight = 14, kBtnDown = 15,
};

enum class Scene {
    Splash, SignIn, LoadingLibrary, Library, Detail, SourcePicker, Settings,
    Accounts, Stream, Fatal
};

enum class LibraryTab { All, OwnedFree, Favorites, History, Consoles };
constexpr int kGameTabCount = 4;
constexpr int kTabCount = 5;  // Consoles only shows with a linked console
constexpr int kHistoryMax = 10;  // recently-played games kept

#ifdef __SWITCH__
// HD-rumble driven straight through libnx. We can't use SDL_JoystickRumble: the
// devkitPro SDL port only initializes vibration handles for HidNpadIdType_No1
// (player 1), never HidNpadIdType_Handheld -- so in handheld mode the send goes
// to a slot with no motor and nothing happens. We instead init both targets
// (two actuators each: left + right) and send to whichever npad is active.
// Unlike SDL, libnx vibration is set-and-hold, so we stop it ourselves once the
// server report's duration elapses (tick()).
struct SwitchRumble {
    HidVibrationDeviceHandle handheld_[2] = {};
    HidVibrationDeviceHandle player1_[2] = {};
    bool ready_ = false;
    bool active_ = false;
    Uint32 expiry_ = 0;

    void init() {
        Result r1 = hidInitializeVibrationDevices(
            handheld_, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        Result r2 = hidInitializeVibrationDevices(
            player1_, 2, HidNpadIdType_No1, HidNpadStyleSet_NpadFullCtrl);
        ready_ = R_SUCCEEDED(r1) && R_SUCCEEDED(r2);
    }

    // Handheld mode -> built-in rails; otherwise the player-1 controller.
    const HidVibrationDeviceHandle* target() const {
        if (hidGetNpadStyleSet(HidNpadIdType_Handheld) &
            HidNpadStyleTag_NpadHandheld)
            return handheld_;
        return player1_;
    }

    void send(float low, float high) {
        auto clamp01 = [](float v) { return v < 0 ? 0.f : (v > 1 ? 1.f : v); };
        HidVibrationValue v[2];
        v[0].amp_low = clamp01(low);
        v[0].freq_low = 160.0f;     // HD-rumble low-band centre
        v[0].amp_high = clamp01(high);
        v[0].freq_high = 320.0f;    // HD-rumble high-band centre
        v[1] = v[0];                // both actuators together
        hidSendVibrationValues(target(), v, 2);
    }

    // Start a burst scaled by the user's intensity gain; auto-stops after
    // duration_ms (floored so a 0-length report is still felt, and the server
    // re-sends to sustain longer effects). The high band gets an extra trim --
    // it is inherently louder/harsher and is what you hear humming.
    void play(float low, float high, float gain, Uint32 duration_ms,
              Uint32 now) {
        if (!ready_) return;
        float lo = low * gain;
        float hi = high * gain * 0.75f;
        send(lo, hi);
        active_ = lo > 0.0f || hi > 0.0f;
        expiry_ = now + (duration_ms ? duration_ms : 150);
    }

    void tick(Uint32 now) {
        if (ready_ && active_ && static_cast<Sint32>(now - expiry_) >= 0) {
            send(0.0f, 0.0f);
            active_ = false;
        }
    }

    void stop() {
        if (ready_) send(0.0f, 0.0f);
        active_ = false;
    }
};
#endif

struct Settings {
    int quality = 2;    // 0=720p, 1=1080p, 2=HQ Windows, 3=HQ Tizen test
    int mapping = 0;    // 0=positional, 1=match labels
    int vibration = 2;  // rumble intensity: 0=Off, 1=Low, 2=Medium, 3=High
    int region = 0;     // region-bypass IP: 0=Off, else index into kRegion*
    std::string server_region;
    int force_region = 1; // 0=Off (Allow Fallback), 1=On (Strict Selected Region Only)
    int max_bitrate = 0;  // 0=Auto, 1=7Mbps, 2=14Mbps, 3=20Mbps, 4=30Mbps
    int language = 0;   // index into kLanguage* (0 = English US)
    int source = 0;     // 0=ask every time, 1=xCloud, 2=your Xbox
    float volume = 1.0f;  // output gain for streamed audio (0.5-4.0); tune in settings.json
    int pacing = 0;     // Presentation pacing: 0=Steady, 1=Smooth
    int console_quality = 0;  // 0=720p stable, 1=1080p experimental
    int picture_profile = 0;  // PictureProfile enum (0=Signal Pure .. 6=Custom)
    int brightness = 0;    // post-process offset: -20..+20
    int contrast = 100;    // post-process multiplier: 70..130 percent
    int saturation = 100;  // post-process multiplier: 0..150 percent
    int gamma = 100;       // midtone curve: 50..200 percent; 100 = 1.00
    int sharpness = 0;  // luma sharpening: 0=Off, 1=Low, 2=Medium, 3=High
    int temperature = 0; // temperature offset: -20..+20
    int debug_hud = 0;  // 0=off, 1=on: on-screen debug overlay while streaming
};

bool is_switch_oled_handheld() {
#ifdef __SWITCH__
    SetSysProductModel model = SetSysProductModel_Iowa;
    if (R_SUCCEEDED(setsysInitialize())) {
        setsysGetProductModel(&model);
        setsysExit();
    }
    bool is_oled = (model == SetSysProductModel_Aula);
    bool is_handheld = (appletGetOperationMode() == AppletOperationMode_Handheld);
    return is_oled && is_handheld;
#else
    return false;
#endif
}

void apply_profile_preset(Settings& settings, int profile) {
    switch (profile) {
        case stream::PictureMidnightCinema:
            settings.brightness = -3;
            settings.contrast = 108;
            settings.saturation = 94;
            settings.gamma = 103;
            settings.sharpness = 1;
            settings.temperature = 2;
            break;
        case stream::PictureSolarEmber:
            settings.brightness = 1;
            settings.contrast = 104;
            settings.saturation = 108;
            settings.gamma = 105;
            settings.sharpness = 1;
            settings.temperature = 12;
            break;
        case stream::PictureRazorEdge:
            settings.brightness = 0;
            settings.contrast = 104;
            settings.saturation = 100;
            settings.gamma = 100;
            settings.sharpness = 3;
            settings.temperature = 0;
            break;
        case stream::PictureNeonPulse:
            settings.brightness = 0;
            settings.contrast = 111;
            settings.saturation = 118;
            settings.gamma = 102;
            settings.sharpness = 2;
            settings.temperature = -2;
            break;
        case stream::PictureOLEDAbyss:
            settings.brightness = -2;
            settings.contrast = 114;
            settings.saturation = 106;
            settings.gamma = 97;
            settings.sharpness = 1;
            settings.temperature = 0;
            break;
        case stream::PictureSignalPure:
        default:
            settings.brightness = 0;
            settings.contrast = 100;
            settings.saturation = 100;
            settings.gamma = 100;
            settings.sharpness = 0;
            settings.temperature = 0;
            break;
    }
}

constexpr int kLanguageCount = 14;
constexpr int kVibrationLevels = 4;
constexpr int kQualityLevels = 4;
constexpr int kPacingLevels = 2;
constexpr int kConsoleQualityLevels = 2;

Settings load_settings();
void save_settings(const Settings& settings);

// A tappable footer hint chip: its screen rect and the button key it maps to.
struct HintHit {
    SDL_Rect rect;
    std::string key;
};

// Short synthesized "tick" for menu navigation. Played through SDL's audren
// backend, which is independent of the stream's audout output, so the two never
// conflict. play() takes an amplitude multiplier (1.0 = the base tick level).
class UiSound {
public:
    bool init() {
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!dev_) return false;
        const int n = 48000 * 16 / 1000;  // ~16 ms
        click_.resize(static_cast<size_t>(n) * 2);
        for (int i = 0; i < n; ++i) {
            float t = static_cast<float>(i) / 48000.0f;
            float s = std::sin(2.0f * 3.14159265f * 1600.0f * t) *
                      std::exp(-t * 140.0f) * 0.09f;  // quiet base level
            auto v = static_cast<int16_t>(s * 32767.0f);
            click_[i * 2] = v;
            click_[i * 2 + 1] = v;
        }
        SDL_PauseAudioDevice(dev_, 0);
        return true;
    }
    void play(float volume) {
        if (!dev_ || click_.empty()) return;
        std::vector<int16_t> buf(click_.size());
        for (size_t i = 0; i < click_.size(); ++i) {
            int v = static_cast<int>(click_[i] * volume);
            buf[i] = v > 32767 ? 32767 : (v < -32768 ? -32768
                                                     : static_cast<int16_t>(v));
        }
        SDL_ClearQueuedAudio(dev_);  // don't pile up on rapid navigation
        SDL_QueueAudio(dev_, buf.data(),
                       static_cast<Uint32>(buf.size() * sizeof(int16_t)));
    }

private:
    SDL_AudioDeviceID dev_ = 0;
    std::vector<int16_t> click_;
};

struct App {
    gfx::Gfx gfx;
    std::vector<HintHit> hint_hits;  // footer chips recorded for touch taps
    UiSound ui_sound;
    std::unique_ptr<Covers> covers;
    std::unique_ptr<XboxAuth> auth;

    Scene scene = Scene::Splash;
    Uint32 scene_started = 0;
    std::string status;   // progress / error line
    std::string fatal;

    // sign-in
    DeviceCode device_code;
    std::atomic<int> signin_state{0};  // 0 running, 1 ok, 2 restart, 3 error
    std::string signin_error;
    std::thread worker;
    std::atomic<bool> abort_http{false};  // exit: unblock worker HTTP calls

    // library
    std::vector<Game> games;
    std::vector<int> visible;  // indices into games for the active tab + search
    std::string query;
    int cursor = 0;
    Uint32 library_focus_started = 0;  // v0.5 card-selection motion
    LibraryTab tab = LibraryTab::All;
    bool library_sidebar_focused = false;
    int library_sidebar_cursor = 0;
    std::vector<std::string> favorites;  // title_ids, marked by the user
    std::vector<std::string> history;    // title_ids, most-recent first
    std::atomic<int> load_state{0};  // 0 running, 1 ok, 2 error
    std::string load_error;
    std::string gamertag;
    Game launch_game;
    Settings settings;
    int settings_cursor = 0;
    int accounts_cursor = 0;  // Scene::Accounts: row in the account picker
    bool remove_armed = false;  // account picker: first X arms, second removes
    Scene settings_return = Scene::Library;  // scene to go back to from Settings
    bool signout_armed = false;  // Settings sign-out row: first A arms, second confirms
    int detail_index = -1;   // games[] index shown in Scene::Detail
    int detail_cursor = 0;   // 0 = Play, 1 = favorite, 2 = Play on... (if any)
    std::vector<HomeConsole> consoles;  // linked Xboxes; empty = hide feature
    std::vector<ServerRegion> server_regions;  // live xCloud datacenters
    bool launching_home = false;        // what the current stream targets
    int console_cursor = 0;             // selected console (list + launches)
    int pick_cursor = 0;                // SourcePicker: 0 xCloud, 1 your Xbox
    Uint32 pick_a_since = 0;            // SourcePicker: A held since (hold=default)
    bool pick_pending = false;          // SourcePicker: A press awaiting release
    std::string last_exit_step;         // previous run's last exit breadcrumb

#ifdef GNX_NATIVE_STREAM
    std::unique_ptr<stream::Engine> engine;
    Uint32 stream_hint_until = 0;
    Uint32 xbox_home_until = 0;  // touchscreen Guide/Nexus press window
    bool quick_menu_open = false;  // in-stream two-dot picture/stats panel
    bool deko_active = false;  // deko3d owns the display (SDL suspended)
    Uint32 last_input_ms = 0;  // input pacing during deko3d streaming
#ifdef __SWITCH__
    SwitchRumble rumble;  // server vibration reports -> HD rumble
#endif
#endif
};

Settings load_settings() {
    Settings settings;
    std::ifstream in(data_path("settings.json"));
    if (!in) return settings;
    json data = json::parse(in, nullptr, false);
    if (data.is_discarded()) return settings;
    settings.quality =
        std::clamp(data.value("quality", 2), 0, kQualityLevels - 1);
    settings.mapping = std::clamp(data.value("mapping", 0), 0, 1);
    // "vibration" was an on/off bool before intensity levels existed; migrate.
    if (data.contains("vibration") && data["vibration"].is_boolean())
        settings.vibration = data["vibration"].get<bool>() ? 2 : 0;
    else
        settings.vibration =
            std::clamp(data.value("vibration", 2), 0, kVibrationLevels - 1);
    settings.region = std::clamp(data.value("region", 0), 0, 5);
    settings.server_region = data.value("server_region", "");
    settings.force_region = std::clamp(data.value("force_region", 1), 0, 1);
    settings.max_bitrate = std::clamp(data.value("max_bitrate", 0), 0, 4);
    settings.language =
        std::clamp(data.value("language", 0), 0, kLanguageCount - 1);
    settings.source = std::clamp(data.value("source", 0), 0, 2);
    settings.volume = std::clamp(data.value("volume", 1.0f), 0.5f, 4.0f);
    // v0.6 stored a bool named "smooth". Prefer the new three-state value,
    // but migrate the old key so users keep their existing choice.
    if (data.contains("pacing"))
        settings.pacing =
            std::clamp(data.value("pacing", 0), 0, 2);
    else
        settings.pacing = data.value("smooth", false) ? 1 : 0;
    if (settings.pacing >= 2) settings.pacing = 1;

    settings.console_quality = std::clamp(
        data.value("console_quality", 0), 0, kConsoleQualityLevels - 1);
    settings.picture_profile = std::clamp(data.value("picture_profile", 0), 0, 6);
    settings.brightness = std::clamp(data.value("brightness", 0), -20, 20);
    settings.contrast = std::clamp(data.value("contrast", 100), 70, 130);
    settings.saturation = std::clamp(data.value("saturation", 100), 0, 150);
    settings.gamma = std::clamp(data.value("gamma", 100), 50, 200);
    settings.sharpness = std::clamp(data.value("sharpness", 0), 0, 3);
    settings.temperature = std::clamp(data.value("temperature", 0), -20, 20);
    settings.debug_hud = std::clamp(data.value("debug_hud", 0), 0, 1);

    if (settings.picture_profile == stream::PictureOLEDAbyss && !is_switch_oled_handheld()) {
        settings.picture_profile = stream::PictureSignalPure;
    }

    if (settings.picture_profile != stream::PictureCustom) {
        apply_profile_preset(settings, settings.picture_profile);
    }

    return settings;
}

void save_settings(const Settings& settings) {
    std::ofstream out(data_path("settings.json"), std::ios::trunc);
    out << json{{"quality", settings.quality},
                {"mapping", settings.mapping},
                {"vibration", settings.vibration},
                {"region", settings.region},
                {"server_region", settings.server_region},
                {"force_region", settings.force_region},
                {"max_bitrate", settings.max_bitrate},
                {"language", settings.language},
                {"source", settings.source},
                {"volume", settings.volume},
                {"pacing", settings.pacing},
                {"smooth", settings.pacing != 0},
                {"console_quality", settings.console_quality},
                {"picture_profile", settings.picture_profile},
                {"brightness", settings.brightness},
                {"contrast", settings.contrast},
                {"saturation", settings.saturation},
                {"gamma", settings.gamma},
                {"sharpness", settings.sharpness},
                {"temperature", settings.temperature},
                {"debug_hud", settings.debug_hud}}.dump(2);
}

// Streamed console's system language (BCP-47). Games without an in-game
// language menu inherit this; sent as the session "locale". Native labels are
// limited to scripts the Switch Standard shared font can render (Latin,
// Cyrillic, Japanese) -- Korean/Chinese need fonts we don't load.
const char* kLanguageLabels[kLanguageCount] = {
    "English (US)", "English (UK)", "Español (España)", "Español (México)",
    "Français", "Deutsch", "Italiano", "Português (Brasil)",
    "Português (Portugal)", "Polski", "Nederlands", "Türkçe",
    "Russian", "Japanese"};
const char* kLanguageCodes[kLanguageCount] = {
    "en-US", "en-GB", "es-ES", "es-MX", "fr-FR", "de-DE", "it-IT",
    "pt-BR", "pt-PT", "pl-PL", "nl-NL", "tr-TR", "ru-RU", "ja-JP"};

// Rumble intensity. HD rumble at amplitude 1.0 is very strong and audibly hums,
// so even "High" leaves headroom rather than driving the actuators flat out.
const char* kVibrationLabels[kVibrationLevels] = {"Off", "Low", "Medium",
                                                  "High"};
const float kVibrationGain[kVibrationLevels] = {0.0f, 0.35f, 0.6f, 0.9f};

// Region bypass: spoof a supported-region IP via X-Forwarded-For so xCloud's
// geo gate opens for accounts outside the officially supported countries. IPs
// are the known-good values shipped by better-xcloud. Index 0 = disabled.
const char* kRegionLabels[6] = {"Off", "United States", "Brazil",
                                "Japan", "Korea", "Poland"};
const char* kRegionIps[6] = {"", "143.244.47.65", "169.150.198.66",
                             "138.199.21.239", "121.125.60.151",
                             "45.134.212.66"};

std::string normalized_server_region(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value)
        if (std::isalnum(ch))
            out.push_back(static_cast<char>(std::toupper(ch)));
    return out;
}

void apply_region(const Settings& settings) {
    // Region bypass and server selection are intentionally independent:
    // bypass changes the location presented to Xbox's geo gate, while
    // server_region selects a real baseUri returned by Xbox. "Off" must never
    // silently spoof an IP merely because a datacenter was selected.
    if (settings.region > 0 && settings.region < 6)
        Http::set_forwarded_for(kRegionIps[settings.region]);
    else
        Http::set_forwarded_for("");
}

std::string pretty_server_region(const std::string& name) {
    struct KnownRegion {
        const char* id;
        const char* country;
        const char* label;
    };
    static const KnownRegion known[] = {
        {"EASTUS", "US", "East US"},
        {"EASTUS2", "US", "East US 2"},
        {"NORTHCENTRALUS", "US", "North Central US"},
        {"SOUTHCENTRALUS", "US", "South Central US"},
        {"WESTUS", "US", "West US"},
        {"WESTUS2", "US", "West US 2"},
        {"WESTUS3", "US", "West US 3"},
        {"MEXICOCENTRAL", "MX", "Mexico Central"},
        {"BRAZILSOUTH", "BR", "Brazil South"},
        {"CHILECENTRAL", "CL", "Chile Central"},
        {"JAPANEAST", "JP", "Japan East"},
        {"KOREACENTRAL", "KR", "Korea Central"},
        {"CENTRALINDIA", "IN", "Central India"},
        {"SOUTHINDIA", "IN", "South India"},
        {"AUSTRALIAEAST", "AU", "Australia East"},
        {"AUSTRALIASOUTHEAST", "AU", "Australia Southeast"},
        {"SWEDENCENTRAL", "SE", "Sweden Central"},
        {"UKSOUTH", "GB", "UK South"},
        {"WESTEUROPE", "NL", "West Europe"},
    };
    std::string key = normalized_server_region(name);
    for (const KnownRegion& region : known)
        if (key == region.id)
            return std::string(region.country) + " · " + region.label;

    // Future Xbox regions still remain readable without waiting for an app
    // update: turn CamelCase into words and keep the stable name as a fallback.
    std::string label;
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(name[i]);
        if (i > 0 && std::isupper(ch) &&
            std::islower(static_cast<unsigned char>(name[i - 1])))
            label.push_back(' ');
        label.push_back(static_cast<char>(ch));
    }
    return label.empty() ? "Unknown region" : label;
}

std::string server_host_code(const std::string& base_uri) {
    size_t start = base_uri.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    size_t end = base_uri.find('.', start);
    if (end == std::string::npos || end <= start) return "";
    std::string code = base_uri.substr(start, end - start);
    for (char& ch : code)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return code;
}

std::string server_region_label(const ServerRegion& region) {
    std::string label = pretty_server_region(region.name);
    std::string host = server_host_code(region.base_uri);
    if (host.empty()) {
        if (normalized_server_region(region.name) == "CHILECENTRAL") host = "CLC";
        else if (normalized_server_region(region.name) == "BRAZILSOUTH")
            host = "BRS";
    }
    return host.empty() ? label : host + " · " + label;
}

std::vector<ServerRegion> load_server_regions() {
    std::ifstream in(data_path("server-regions.json"));
    if (!in) return {};
    json data = json::parse(in, nullptr, false);
    if (!data.is_array()) return {};

    std::vector<ServerRegion> regions;
    for (const json& item : data) {
        ServerRegion region;
        region.name = item.value("name", "");
        region.base_uri = item.value("base_uri", "");
        region.is_default = item.value("is_default", false);
        if (!region.name.empty()) regions.push_back(std::move(region));
    }
    return regions;
}

void save_server_regions(const std::vector<ServerRegion>& regions) {
    json data = json::array();
    for (const ServerRegion& region : regions)
        data.push_back({{"name", region.name},
                        {"base_uri", region.base_uri},
                        {"is_default", region.is_default}});
    std::ofstream out(data_path("server-regions.json"), std::ios::trunc);
    out << data.dump(2);
}

bool same_server_regions(const std::vector<ServerRegion>& left,
                         const std::vector<ServerRegion>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i)
        if (left[i].name != right[i].name ||
            left[i].base_uri != right[i].base_uri ||
            left[i].is_default != right[i].is_default)
            return false;
    return true;
}

void sync_server_regions(App& app) {
    std::vector<ServerRegion> live = app.auth->available_cloud_regions();
    if (live.empty() || same_server_regions(live, app.server_regions)) return;
    app.server_regions = std::move(live);
    save_server_regions(app.server_regions);
}

std::vector<ServerRegion> server_region_choices(const App& app) {
    // Keep the two useful South-American choices visible even before the first
    // online refresh. Their endpoints are never guessed: the auth response
    // still has to contain the matching stable name before it can be selected.
    std::vector<ServerRegion> choices = {
        {"ChileCentral", "", false}, {"BrazilSouth", "", false}};
    auto merge = [&choices](const ServerRegion& incoming) {
        const std::string key = normalized_server_region(incoming.name);
        for (ServerRegion& existing : choices)
            if (normalized_server_region(existing.name) == key) {
                if (!incoming.base_uri.empty()) existing = incoming;
                return;
            }
        choices.push_back(incoming);
    };
    for (const ServerRegion& region : app.server_regions) merge(region);
    if (!app.settings.server_region.empty())
        merge({app.settings.server_region, "", false});
    return choices;
}

std::string selected_server_region_label(const App& app) {
    if (app.settings.server_region.empty()) {
        for (const ServerRegion& region : app.server_regions)
            if (region.is_default) {
                return "Auto · " + server_region_label(region);
            }
        return "Auto";
    }
    for (const ServerRegion& region : server_region_choices(app))
        if (normalized_server_region(region.name) ==
            normalized_server_region(app.settings.server_region))
            return server_region_label(region);
    return pretty_server_region(app.settings.server_region);
}

void cycle_server_region(App& app, int direction) {
    std::vector<ServerRegion> choices = server_region_choices(app);
    int current = 0;  // 0 = Auto; returned regions start at 1.
    for (size_t i = 0; i < choices.size(); ++i)
        if (normalized_server_region(choices[i].name) ==
            normalized_server_region(app.settings.server_region)) {
            current = static_cast<int>(i) + 1;
            break;
        }
    const int count = static_cast<int>(choices.size()) + 1;
    current = (current + direction + count) % count;
    app.settings.server_region =
        current == 0 ? "" : choices[static_cast<size_t>(current - 1)].name;
    app.auth->set_preferred_server_region(app.settings.server_region);
}

// ---- persistence ----------------------------------------------------------

// v5 records alternative-catalog membership for the Owned & Free tab.
constexpr int kGamesCacheVersion = 5;

void save_games_cache(const std::vector<Game>& games) {
    json list = json::array();
    for (const Game& game : games)
        list.push_back({{"titleId", game.title_id},
                        {"productId", game.product_id},
                        {"name", game.name},
                        {"boxArt", game.box_art_url},
                        {"f2pOffering", game.uses_f2p_offering},
                        {"f2pCatalog", game.available_on_f2p}});
    std::ofstream out(user_path("games.json"), std::ios::trunc);
    out << json{{"version", kGamesCacheVersion}, {"games", list}}.dump();
}

std::vector<Game> load_games_cache() {
    std::vector<Game> games;
    std::ifstream in(user_path("games.json"));
    if (!in) return games;
    json data = json::parse(in, nullptr, false);
    if (data.is_discarded() || !data.is_object() ||
        data.value("version", 0) != kGamesCacheVersion)
        return games;  // stale or old-format cache -> full refresh
    for (const json& entry : data.value("games", json::array())) {
        Game game;
        game.title_id = entry.value("titleId", "");
        game.product_id = entry.value("productId", "");
        game.name = entry.value("name", "");
        game.box_art_url = entry.value("boxArt", "");
        game.uses_f2p_offering = entry.value("f2pOffering", false);
        game.available_on_f2p = entry.value("f2pCatalog", false);
        if (!game.title_id.empty()) games.push_back(std::move(game));
    }
    return games;
}

// Linked consoles are cached like the games list, so the source picker is
// available on cache-served boots too; refreshed on every full library load.
void save_consoles_cache(const std::vector<HomeConsole>& consoles) {
    json list = json::array();
    for (const HomeConsole& console : consoles)
        list.push_back({{"serverId", console.server_id},
                        {"name", console.name},
                        {"consoleType", console.console_type},
                        {"powerState", console.power_state}});
    std::ofstream out(user_path("consoles.json"), std::ios::trunc);
    out << json{{"consoles", list}}.dump();
}

std::vector<HomeConsole> load_consoles_cache() {
    std::vector<HomeConsole> consoles;
    std::ifstream in(user_path("consoles.json"));
    if (!in) return consoles;
    json data = json::parse(in, nullptr, false);
    if (data.is_discarded() || !data.is_object()) return consoles;
    for (const json& entry : data.value("consoles", json::array())) {
        HomeConsole console;
        console.server_id = entry.value("serverId", "");
        console.name = entry.value("name", "");
        console.console_type = entry.value("consoleType", "");
        console.power_state = entry.value("powerState", "");
        if (!console.server_id.empty()) consoles.push_back(std::move(console));
    }
    return consoles;
}

// Favorites and history are stored as plain title-id lists (JSON arrays).
std::vector<std::string> load_id_list(const char* leaf) {
    std::vector<std::string> ids;
    std::ifstream in(user_path(leaf));
    if (!in) return ids;
    json data = json::parse(in, nullptr, false);
    if (!data.is_array()) return ids;
    for (const json& entry : data)
        if (entry.is_string()) ids.push_back(entry.get<std::string>());
    return ids;
}

void save_id_list(const char* leaf, const std::vector<std::string>& ids) {
    std::ofstream out(user_path(leaf), std::ios::trunc);
    out << json(ids).dump();
}

bool is_favorite(const App& app, const std::string& id) {
    return std::find(app.favorites.begin(), app.favorites.end(), id) !=
           app.favorites.end();
}

void toggle_favorite(App& app, const std::string& id) {
    auto it = std::find(app.favorites.begin(), app.favorites.end(), id);
    if (it != app.favorites.end())
        app.favorites.erase(it);
    else
        app.favorites.push_back(id);
    save_id_list("favorites.json", app.favorites);
}

// Record a launch: move the title to the front, dedup, cap at kHistoryMax.
void push_history(App& app, const std::string& id) {
    auto it = std::find(app.history.begin(), app.history.end(), id);
    if (it != app.history.end()) app.history.erase(it);
    app.history.insert(app.history.begin(), id);
    if (static_cast<int>(app.history.size()) > kHistoryMax)
        app.history.resize(kHistoryMax);
    save_id_list("history.json", app.history);
}

// ---- background work ------------------------------------------------------

void start_signin(App& app) {
    app.signin_state = 0;
    app.worker = std::thread([&app] {
        try {
            app.device_code = app.auth->request_device_code();
        } catch (const std::exception& error) {
            app.signin_error = error.what();
            app.signin_state = 3;
            return;
        }
        while (app.signin_state == 0) {
            // Sleep in short slices so a cancel (exit) doesn't have to wait
            // out the full poll interval (5-15 s) before the join returns.
            Uint32 wait_ms = static_cast<Uint32>(
                std::max(app.device_code.interval_secs, 1) * 1000);
            for (Uint32 waited = 0; waited < wait_ms && app.signin_state == 0;
                 waited += 100)
                SDL_Delay(100);
            if (app.signin_state != 0) return;
            try {
                switch (app.auth->poll_device_code(app.device_code)) {
                    case PollResult::Authorized: app.signin_state = 1; return;
                    case PollResult::Expired:    app.signin_state = 2; return;
                    case PollResult::Pending:    break;
                }
            } catch (const std::exception& error) {
                app.signin_error = error.what();
                app.signin_state = 3;
                return;
            }
        }
    });
}

void start_library_load(App& app, bool force_refresh) {
    app.load_state = 0;
    app.status = "Connecting to Xbox...";
    app.worker = std::thread([&app, force_refresh] {
        try {
            if (!force_refresh) {
                std::vector<Game> cached = load_games_cache();
                if (!cached.empty()) {
                    app.games = std::move(cached);
                    app.consoles = load_consoles_cache();
                    app.load_state = 1;
                    return;
                }
            }
            app.status = "Fetching streaming credentials...";
            StreamingCredentials credentials =
                app.auth->fetch_streaming_credentials();
            app.server_regions = credentials.cloud.regions;
            save_server_regions(app.server_regions);
            app.status = "Loading all playable cloud games...";
            Http http;
            // Linked consoles for xHome remote play. Non-fatal: no consoles
            // (or an error here) just leaves the source picker hidden. The
            // outcome lands in consoles-log.txt so "option not showing" is
            // diagnosable from the SD card.
            std::string console_log;
            try {
                app.consoles = fetch_home_consoles(http, credentials.home);
                save_consoles_cache(app.consoles);
                console_log = "found " + std::to_string(app.consoles.size()) +
                              " console(s)";
                for (const HomeConsole& c : app.consoles)
                    console_log += "\n  " + c.name + " [" + c.console_type +
                                   "] power=" + c.power_state;
            } catch (const std::exception& error) {
                app.consoles.clear();
                console_log = std::string("console list failed: ") +
                              error.what();
            }
            if (FILE* f = std::fopen(data_path("consoles-log.txt").c_str(),
                                     "w")) {
                std::fprintf(f, "%s\n", console_log.c_str());
                std::fclose(f);
            }
            std::vector<Game> games =
                fetch_playable_titles(http, credentials);
            app.status = "Resolving names and covers (" +
                         std::to_string(games.size()) + " titles)...";
            fetch_names(http, games);
            std::sort(games.begin(), games.end(),
                      [](const Game& a, const Game& b) {
                          const std::string& left =
                              a.name.empty() ? a.title_id : a.name;
                          const std::string& right =
                              b.name.empty() ? b.title_id : b.name;
                          return left < right;
                      });
            save_games_cache(games);
            app.games = std::move(games);
            app.load_state = 1;
        } catch (const std::exception& error) {
            app.load_error = error.what();
            app.load_state = 2;
        }
    });
}

void join_worker(App& app) {
    if (app.worker.joinable()) app.worker.join();
}

// ---- helpers --------------------------------------------------------------

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

int find_game(const App& app, const std::string& id) {
    for (int i = 0; i < static_cast<int>(app.games.size()); ++i)
        if (app.games[i].title_id == id) return i;
    return -1;
}

// Rebuild `visible` for the active tab, honouring the search query. All /
// Favorites keep the library's alphabetical order; History keeps recency order.
// Hand the app over to another account: repoint the token store, drop the
// previous account's library from memory, and either load the new account's
// library or ask it to sign in. Box art is shared, so switching is cheap.
void switch_account(App& app, const std::string& id) {
    std::string previous = g_active_account;
    if (app.worker.joinable()) {  // a sign-in poll may still be running
        app.signin_state = 4;     // cancel
        app.abort_http = true;    // unblock an in-flight request
        join_worker(app);
        app.abort_http = false;   // ... and re-arm HTTP for the new account
    }
    g_active_account = id;
    make_account_dir(id);
    save_accounts();
    app.auth->set_token_store(user_path("tokens.json"));
    app.games.clear();
    app.visible.clear();
    app.consoles.clear();
    app.favorites = load_id_list("favorites.json");
    app.history = load_id_list("history.json");
    app.gamertag.clear();
    app.cursor = app.console_cursor = 0;
    app.tab = LibraryTab::All;
    app.query.clear();
    if (app.auth->has_saved_login()) {
        g_signin_return_account.clear();
        app.scene = Scene::LoadingLibrary;
        start_library_load(app, false);
    } else {
        // Signing in for an account we switched to: B on the sign-in screen
        // must come back here, not quit the app (which is what it means when
        // sign-in is the first screen of a fresh install). Only worth offering
        // if there is actually something to go back to.
        if (previous != id && account_exists(previous) &&
            account_has_login(previous))
            g_signin_return_account = previous;
        else
            g_signin_return_account.clear();
        app.scene = Scene::SignIn;
        start_signin(app);
    }
}

void add_account(App& app) {
    g_accounts.push_back({next_account_id(), ""});
    switch_account(app, g_accounts.back().id);
}

void apply_filter(App& app) {
    app.visible.clear();
    std::string needle = lowercase(app.query);
    auto matches = [&](const Game& game) {
        return needle.empty() ||
               lowercase(game.name).find(needle) != std::string::npos ||
               lowercase(game.title_id).find(needle) != std::string::npos;
    };

    if (app.tab == LibraryTab::Consoles) {
        return;  // the Consoles tab lists consoles, not games
    } else if (app.tab == LibraryTab::History) {
        for (const std::string& id : app.history) {
            int i = find_game(app, id);
            if (i >= 0 && matches(app.games[i])) app.visible.push_back(i);
        }
    } else {
        for (int i = 0; i < static_cast<int>(app.games.size()); ++i) {
            if (app.tab == LibraryTab::OwnedFree &&
                !app.games[i].available_on_f2p)
                continue;
            if (app.tab == LibraryTab::Favorites &&
                !is_favorite(app, app.games[i].title_id))
                continue;
            if (matches(app.games[i])) app.visible.push_back(i);
        }
    }
    app.cursor = std::min(app.cursor,
                          std::max(0, static_cast<int>(app.visible.size()) - 1));
}

std::string keyboard_input(const std::string& initial) {
#ifdef __SWITCH__
    SwkbdConfig keyboard;
    char buffer[256] = {};
    if (R_FAILED(swkbdCreate(&keyboard, 0))) return initial;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetGuideText(&keyboard, "Search your library");
    swkbdConfigSetInitialText(&keyboard, initial.c_str());
    Result rc = swkbdShow(&keyboard, buffer, sizeof(buffer));
    swkbdClose(&keyboard);
    return R_SUCCEEDED(rc) ? std::string(buffer) : initial;
#else
    return initial.empty() ? "halo" : "";  // desktop stub for testing
#endif
}

// ---- scenes (Light is Green v1.0 native layout) ---------------------------

constexpr int kMargin = 60;    // TV-safe margin on all edges
constexpr int kFooterH = 84;
constexpr int kFooterY = gfx::kHeight - kFooterH;
#ifdef GNX_NATIVE_STREAM
// Compact 48x48 physical-pixel touch target in handheld mode. It sits eight
// design pixels from the two-dot control and contains only the Xbox symbol.
constexpr SDL_Rect kXboxHomeRect = {
    stream::kGuideButtonRect.x, stream::kGuideButtonRect.y,
    stream::kGuideButtonRect.w, stream::kGuideButtonRect.h};

bool point_in_rect(int x, int y, const SDL_Rect& rect) {
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y &&
           y < rect.y + rect.h;
}

bool point_in_rect(int x, int y, const stream::QuickRect& rect) {
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y &&
           y < rect.y + rect.h;
}

bool xbox_home_active(const App& app) {
    return SDL_GetTicks() < app.xbox_home_until;
}

void draw_xbox_home_button(App& app) {
    const bool pressed = xbox_home_active(app);
    SDL_Renderer* renderer = app.gfx.renderer();
    const int cx = kXboxHomeRect.x + kXboxHomeRect.w / 2;
    const int cy = kXboxHomeRect.y + kXboxHomeRect.h / 2;
    auto circle = [&](int radius, gfx::Color color, int y_offset = 0) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        for (int dy = -radius; dy <= radius; ++dy) {
            int half = static_cast<int>(
                std::sqrt(radius * radius - dy * dy));
            SDL_RenderDrawLine(renderer, cx - half, cy + y_offset + dy,
                               cx + half, cy + y_offset + dy);
        }
    };

    // Symbol only. The 72x72 rect remains the touch target, but the old green
    // square and border are intentionally not painted.
    circle(22, {0, 0, 0,
                static_cast<Uint8>(pressed ? 180 : 125)}, 2);
    const int radius = pressed ? 20 : 18;
    gfx::Color sphere = pressed ? gfx::kText : gfx::kAccent;
    circle(radius, sphere);
    gfx::Color mark = pressed ? gfx::kAccent : gfx::kText;
    SDL_SetRenderDrawColor(renderer, mark.r, mark.g, mark.b, mark.a);
    for (int t = -2; t <= 2; ++t) {
        SDL_RenderDrawLine(renderer, cx - 10, cy - 11 + t, cx + 10,
                           cy + 11 + t);
        SDL_RenderDrawLine(renderer, cx + 10, cy - 11 + t, cx - 10,
                           cy + 11 + t);
    }
}
#endif

// One footer hint: a button chip plus its label. The screen's primary action
// gets the solid-accent chip.
struct Hint {
    const char* key;
    const char* label;
    bool primary = false;
};

// Chip = 40x40 fill (wider for multi-char keys) + 2px frame + centered glyph.
int chip_width(App& app, const std::string& key) {
    int tw = app.gfx.text_width(key, gfx::FontSize::Small);
    return std::max(40, tw + 16);
}

void draw_chip(App& app, const std::string& key, int x, int y, bool primary) {
    SDL_Rect box = {x, y, chip_width(app, key), 40};
    app.gfx.fill(box, primary ? gfx::kAccent : gfx::kChip);
    if (!primary) app.gfx.frame(box, gfx::kChipEdge, 2);
    app.gfx.text_centered(key, box.x + box.w / 2, y + 4, gfx::FontSize::Small,
                          primary ? gfx::kText : gfx::kText);
}

// Right-aligned hint row. with_bar draws the footer band behind it; screens
// over video (stream) keep the background clear.
void draw_hints(App& app, const std::vector<Hint>& hints,
                bool with_bar = true) {
    if (with_bar) {
        app.gfx.fill({0, kFooterY, gfx::kWidth, kFooterH}, gfx::kBar);
        app.gfx.fill({0, kFooterY, gfx::kWidth, 2}, gfx::kChip);
    }
    app.hint_hits.clear();
    int x = gfx::kWidth - kMargin;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        int lw = app.gfx.text_width(it->label, gfx::FontSize::Small);
        int cw = chip_width(app, it->key);
        x -= cw + 12 + lw;
        app.hint_hits.push_back({{x, kFooterY, cw + 12 + lw, kFooterH}, it->key});
        int cy = kFooterY + (kFooterH - 40) / 2;
        draw_chip(app, it->key, x, cy, it->primary);
        app.gfx.text(it->label, x + cw + 12, cy + 4, gfx::FontSize::Small,
                     it->primary ? gfx::kText : gfx::kTextDim);
        x -= 32;
    }
}

// Focus system: a compact animated emerald edge and two cheap translucent
// frames. This gives the selected game motion without a blur texture or a
// per-pixel post-process on Switch.
void draw_focus_frames(App& app, const SDL_Rect& r) {
    const float pulse = 0.5f + 0.5f * std::sin(SDL_GetTicks() / 260.0f);
    app.gfx.frame(r, gfx::kFocus, 4);
    app.gfx.frame({r.x - 5, r.y - 5, r.w + 10, r.h + 10},
                  {gfx::kFocus.r, gfx::kFocus.g, gfx::kFocus.b,
                   static_cast<Uint8>(66 + 54 * pulse)},
                  4);
    app.gfx.frame({r.x - 11, r.y - 11, r.w + 22, r.h + 22},
                  {gfx::kFocus.r, gfx::kFocus.g, gfx::kFocus.b,
                   static_cast<Uint8>(20 + 30 * pulse)},
                  5);
}

// Compact command-deck header shared by the library and secondary screens.
void draw_header(App& app) {
    app.gfx.fill({0, 0, gfx::kWidth, 108}, gfx::kBar);
    app.gfx.fill({0, 104, gfx::kWidth, 2}, {40, 118, 103, 165});
    app.gfx.fill({0, 106, gfx::kWidth, 2}, {0, 0, 0, 96});
    SDL_Rect icon = {kMargin, 16, 76, 76};
    app.gfx.draw_brand_icon(icon);
    app.gfx.text("Light is Green", kMargin + 98, 20, gfx::FontSize::Body,
                 gfx::kText);
    app.gfx.text(std::string("V") + GNX_VERSION + "  /  CLOUD LIBRARY",
                 kMargin + 98, 64, gfx::FontSize::Small, gfx::kFocus);

    const bool signed_in = !app.gamertag.empty();
    const std::string player = signed_in ? app.gamertag : "PLAYER";
    const std::string status = signed_in ? "CLOUD READY" : "SIGN IN";
    const int status_width = app.gfx.text_width(status, gfx::FontSize::Small);
    const int player_width = app.gfx.text_width(player, gfx::FontSize::Small);
    const int right = gfx::kWidth - kMargin;
    app.gfx.fill({right - 16, 36, 9, 9}, signed_in ? gfx::kFocus : gfx::kFaint);
    app.gfx.text(status, right - 30 - status_width, 56, gfx::FontSize::Small,
                 signed_in ? gfx::kFocus : gfx::kFaint);
    app.gfx.text(player, right - player_width, 20, gfx::FontSize::Small,
                 gfx::kText);
}

// Fallback cover: per-title dark hue + big translucent initials, so a grid
// with covers still downloading reads as content instead of gray boxes.
const gfx::Color kCoverHues[12] = {
    {35, 48, 71},  {58, 36, 48},  {31, 58, 51},  {59, 50, 32},
    {44, 36, 64},  {64, 38, 32},  {32, 48, 63},  {51, 32, 44},
    {36, 49, 58},  {49, 42, 30},  {30, 44, 36},  {46, 34, 51}};

gfx::Color cover_hue(const std::string& title_id) {
    unsigned hash = 5381;
    for (unsigned char c : title_id) hash = hash * 33 + c;
    return kCoverHues[hash % 12];
}

std::string cover_initials(const Game& game) {
    const std::string& name = game.name.empty() ? game.title_id : game.name;
    std::string initials;
    bool word_start = true;
    for (char c : name) {
        if (c == ' ') { word_start = true; continue; }
        if (word_start && initials.size() < 2)
            initials += static_cast<char>(std::toupper(c));
        word_start = false;
    }
    return initials;
}

void draw_cover_fallback(App& app, const Game& game, const SDL_Rect& rect,
                         gfx::FontSize initial_size) {
    app.gfx.fill(rect, cover_hue(game.title_id));
    app.gfx.text_centered(cover_initials(game), rect.x + rect.w / 2,
                          rect.y + rect.h / 2 - 40, initial_size,
                          {255, 255, 255, 36});
}

void draw_splash(App& app) {
    Uint32 elapsed = SDL_GetTicks() - app.scene_started;
    int logo_w = static_cast<int>(120 * std::min(elapsed / 300.0f, 1.0f));
    int word_w = app.gfx.text_width("Light is Green", gfx::FontSize::Huge);
    int row_w = 120 + 36 + word_w;
    int x0 = (gfx::kWidth - row_w) / 2;
    if (logo_w > 0) {
        app.gfx.draw_brand_icon({x0, 400, logo_w, 120});
    }
    if (elapsed > 300) {
        app.gfx.text("Light is Green", x0 + 156, 408, gfx::FontSize::Huge,
                     gfx::kText);
        app.gfx.text_centered("Cloud gaming for Nintendo Switch",
                              gfx::kWidth / 2, 548, gfx::FontSize::Note,
                              gfx::kTextDim);
        app.gfx.text_centered("OFFICIAL V1.0", gfx::kWidth / 2, 594,
                              gfx::FontSize::Small, gfx::kFocus);
    }
    // The bar IS the boot indicator: no spinner (card 1b).
    SDL_Rect track = {gfx::kWidth / 2 - 180, 648, 360, 6};
    app.gfx.fill(track, gfx::kChip);
    int progress = static_cast<int>(360 * std::min(elapsed / 1200.0f, 1.0f));
    app.gfx.fill({track.x, track.y, progress, 6}, gfx::kFocus);
}

// Numbered step bullet used by the sign-in screen.
void draw_step_box(App& app, const char* number, int x, int y) {
    SDL_Rect box = {x, y, 44, 44};
    app.gfx.fill(box, gfx::kSurface);
    app.gfx.frame(box, gfx::kChipEdge, 2);
    app.gfx.text_centered(number, x + 22, y + 6, gfx::FontSize::Small,
                          gfx::kTextDim);
}

// The device code split in two groups of glyphs drawn with a fixed 14px
// extra advance (readable at 3 m, card 1c).
int spaced_code_width(App& app, const std::string& group) {
    int w = 0;
    for (char c : group)
        w += app.gfx.text_width(std::string(1, c), gfx::FontSize::Huge) + 14;
    return w > 0 ? w - 14 : 0;
}

int draw_spaced_code(App& app, const std::string& group, int x, int y) {
    for (char c : group) {
        x += app.gfx.text(std::string(1, c), x, y, gfx::FontSize::Huge,
                          gfx::kText) + 14;
    }
    return x;
}

void draw_signin(App& app) {
    draw_header(app);
    app.gfx.text_centered("Sign in with Microsoft", gfx::kWidth / 2, 170,
                          gfx::FontSize::Title, gfx::kText);
    if (app.device_code.user_code.empty()) {
        app.gfx.spinner(gfx::kWidth / 2, 480, SDL_GetTicks());
        draw_hints(app, {{"B", "Exit"}});
        return;
    }

    // Step 1: box + text + URL chip, centered as one row.
    const std::string& uri = app.device_code.verification_uri;
    int uri_w = app.gfx.text_width(uri, gfx::FontSize::Body) + 56;
    int t1_w = app.gfx.text_width("On your phone or computer, open",
                                  gfx::FontSize::Note);
    int row1_w = 44 + 24 + t1_w + 24 + uri_w;
    int x = (gfx::kWidth - row1_w) / 2;
    draw_step_box(app, "1", x, 300);
    app.gfx.text("On your phone or computer, open", x + 68, 306,
                 gfx::FontSize::Note, gfx::kTextDim);
    SDL_Rect uri_box = {x + 68 + t1_w + 24, 292, uri_w, 62};
    app.gfx.fill(uri_box, gfx::kSurface);
    app.gfx.text_centered(uri, uri_box.x + uri_box.w / 2, 300,
                          gfx::FontSize::Body, gfx::kFocus);

    // Step 2.
    int t2_w = app.gfx.text_width("and enter this code", gfx::FontSize::Note);
    x = (gfx::kWidth - (44 + 24 + t2_w)) / 2;
    draw_step_box(app, "2", x, 392);
    app.gfx.text("and enter this code", x + 68, 398, gfx::FontSize::Note,
                 gfx::kTextDim);

    // The code, split 4+4 (or at the dash) to avoid read errors.
    std::string code = app.device_code.user_code;
    std::string left = code, right;
    size_t dash = code.find('-');
    if (dash != std::string::npos) {
        left = code.substr(0, dash);
        right = code.substr(dash + 1);
    } else if (code.size() > 4) {
        left = code.substr(0, code.size() / 2);
        right = code.substr(code.size() / 2);
    }
    int lw = spaced_code_width(app, left);
    int rw = spaced_code_width(app, right);
    int inner = lw + (right.empty() ? 0 : 48 + 24 + 48 + rw);
    SDL_Rect box = {(gfx::kWidth - (inner + 160)) / 2, 480, inner + 160, 190};
    app.gfx.fill(box, gfx::kSurface);
    app.gfx.frame(box, gfx::kAccent, 4);
    int cx = box.x + 80;
    cx = draw_spaced_code(app, left, cx, box.y + 34);
    if (!right.empty()) {
        app.gfx.fill({cx + 48, box.y + box.h / 2 - 4, 24, 8}, gfx::kChipEdge);
        draw_spaced_code(app, right, cx + 48 + 24 + 48, box.y + 34);
    }

    app.gfx.spinner(gfx::kWidth / 2 - 140, 736, SDL_GetTicks());
    app.gfx.text("Waiting for you to sign in…", gfx::kWidth / 2 - 90, 728,
                 gfx::FontSize::Small, gfx::kTextDim);
    draw_hints(app, {{"ZL", "Settings · region bypass"}, {"B", "Exit"}});
}

// v1.0.3 sidebar preview: the persistent controller-first rail replaces both
// top navigation rows. The content deck keeps three complete rows and six
// full-bleed cards per row without crowding the focus glow or touch targets.
constexpr int kColumns = 6;
constexpr int kCardW = 250;
constexpr int kCardH = 214;
constexpr int kGapX = 12;
constexpr int kGapY = 12;
constexpr int kGridX = 292;
constexpr int kGridY = 280;
constexpr int kRowsVisible = 3;
constexpr int kPageSize = kColumns * kRowsVisible;
constexpr SDL_Rect kLibraryHero{60, 196, 1800, 450};
constexpr SDL_Rect kLibraryPanel{276, 124, 1584, 840};
constexpr SDL_Rect kLibrarySidebar{34, 124, 226, 840};
constexpr int kSidebarItemX = 48;
constexpr int kSidebarItemW = 198;
constexpr int kSidebarItemH = 66;
constexpr int kSidebarTabY = 188;
constexpr int kSidebarItemGap = 12;
constexpr int kSidebarSearchY = 612;
constexpr int kSidebarSettingsY = 690;
constexpr int kConsoleGridX = 292;
constexpr int kConsoleGridY = 280;
constexpr int kConsoleCardW = 500;
constexpr int kConsoleCardH = 380;
constexpr int kConsoleGapX = 30;
constexpr Uint32 kLibraryFocusMotionMs = 220;

float library_focus_motion(const App& app) {
    if (app.library_focus_started == 0) return 1.0f;
    float t = std::clamp(
        (SDL_GetTicks() - app.library_focus_started) /
            static_cast<float>(kLibraryFocusMotionMs),
        0.0f, 1.0f);
    // Ease-out-back gives selection a brief, restrained spring without ever
    // blocking input or changing the grid's layout bounds.
    float u = t - 1.0f;
    return 1.0f + 2.70158f * u * u * u + 1.70158f * u * u;
}

SDL_Rect scale_about_center(const SDL_Rect& rect, float scale) {
    int w = static_cast<int>(std::lround(rect.w * scale));
    int h = static_cast<int>(std::lround(rect.h * scale));
    return {rect.x - (w - rect.w) / 2, rect.y - (h - rect.h) / 2, w, h};
}

const char* kTabNames[kTabCount] = {"All games", "Owned & Free", "Favorites",
                                    "History", "Consoles"};

const char* kQualityLabels[kQualityLevels] = {
    "720p", "1080p", "1080p HQ · Windows", "1080p HQ · Tizen test"};
const char* kConsoleQualityLabels[kConsoleQualityLevels] = {
    "720p · Stable", "1080p · Experimental"};
const char* kPacingLabels[kPacingLevels] = {
    "Steady", "Smooth"};
const char* kMappingLabels[2] = {"Positional (Switch A = Xbox B)",
                                 "Match labels (Switch A = Xbox A)"};
const char* kSharpnessLabels[4] = {"Off", "Low", "Medium", "High"};

int library_tab_count(const App& app) {
    return app.consoles.empty() ? kGameTabCount : kTabCount;
}

SDL_Rect library_sidebar_tab_rect(int index) {
    return {kSidebarItemX,
            kSidebarTabY + index * (kSidebarItemH + kSidebarItemGap),
            kSidebarItemW, kSidebarItemH};
}

SDL_Rect library_sidebar_action_rect(int index, int tabs) {
    return {kSidebarItemX,
            index == tabs ? kSidebarSearchY : kSidebarSettingsY,
            kSidebarItemW, kSidebarItemH};
}

void activate_library_tab(App& app, int index) {
    const int tabs = library_tab_count(app);
    if (index < 0 || index >= tabs) return;
    app.tab = static_cast<LibraryTab>(index);
    app.cursor = 0;
    app.console_cursor = 0;
    app.library_sidebar_cursor = index;
    apply_filter(app);
}

void draw_library_sidebar_item(App& app, const SDL_Rect& item,
                               const char* label, bool active, bool focused,
                               const char* shortcut = nullptr) {
    const gfx::Color idle = {4, 24, 26, 224};
    const gfx::Color selected = {0, 121, 83, 116};
    app.gfx.fill(item, focused ? gfx::kSurfaceHi : active ? selected : idle);
    app.gfx.frame(item, focused ? gfx::kFocus
                                : active ? gfx::Color{45, 244, 165, 165}
                                         : gfx::Color{43, 104, 94, 125},
                  focused ? 3 : 1);
    app.gfx.fill({item.x + 16, item.y + 29, 12, 12},
                 active || focused ? gfx::kFocus : gfx::kFaint);
    // Keep localized/long labels inside the rail and away from the shortcut.
    SDL_Rect label_clip = {item.x + 40, item.y + 4,
                           item.w - 52 - (shortcut ? 34 : 0), item.h - 8};
    SDL_RenderSetClipRect(app.gfx.renderer(), &label_clip);
    app.gfx.text(label, item.x + 44, item.y + 19, gfx::FontSize::Small,
                 active || focused ? gfx::kText : gfx::kTextDim);
    SDL_RenderSetClipRect(app.gfx.renderer(), nullptr);
    if (shortcut) {
        const int shortcut_w =
            app.gfx.text_width(shortcut, gfx::FontSize::Small);
        app.gfx.text(shortcut, item.x + item.w - shortcut_w - 16,
                     item.y + 19, gfx::FontSize::Small,
                     focused ? gfx::kFocus : gfx::kFaint);
    }
    if (active)
        app.gfx.fill({item.x + item.w - 6, item.y + 12, 4, item.h - 24},
                     gfx::kFocus);
}

void draw_library_sidebar(App& app, bool settings_active = false,
                          bool loading = false) {
    app.gfx.fill(kLibrarySidebar, {2, 14, 15, 232});
    app.gfx.frame(kLibrarySidebar, {43, 104, 94, 170}, 2);
    app.gfx.text("BROWSE", kSidebarItemX + 4, 145, gfx::FontSize::Small,
                 gfx::kFocus);

    // Loading runs concurrently with the catalog worker. Use a static rail in
    // that scene so rendering never reads vectors while the worker replaces
    // them.
    const int tabs = loading ? kGameTabCount : library_tab_count(app);
    for (int i = 0; i < tabs; ++i) {
        draw_library_sidebar_item(
            app, library_sidebar_tab_rect(i), kTabNames[i],
            !loading && !settings_active && static_cast<int>(app.tab) == i,
            !loading && !settings_active && app.library_sidebar_focused &&
                app.library_sidebar_cursor == i);
    }

    app.gfx.fill({kSidebarItemX + 8, 600, kSidebarItemW - 16, 2},
                 {43, 104, 94, 130});
    const int search_index = tabs;
    const int settings_index = tabs + 1;
    draw_library_sidebar_item(
        app, library_sidebar_action_rect(search_index, tabs), "Search",
        !loading && !settings_active && !app.query.empty(),
        !loading && !settings_active && app.library_sidebar_focused &&
            app.library_sidebar_cursor == search_index,
        "Y");
    draw_library_sidebar_item(
        app, library_sidebar_action_rect(settings_index, tabs), "Settings",
        !loading && settings_active,
        !loading && !settings_active && app.library_sidebar_focused &&
            app.library_sidebar_cursor == settings_index,
        "ZL");

    SDL_Rect status = {kSidebarItemX, 790, kSidebarItemW, 150};
    app.gfx.fill(status, {4, 31, 31, 224});
    app.gfx.frame(status, {43, 104, 94, 145}, 1);
    app.gfx.fill({status.x + 18, status.y + 22, 12, 12}, gfx::kFocus);
    SDL_Rect status_title_clip = {status.x + 40, status.y + 4,
                                  status.w - 54, 42};
    SDL_RenderSetClipRect(app.gfx.renderer(), &status_title_clip);
    app.gfx.text(loading ? "SYNCING" : "CLOUD READY", status.x + 44,
                 status.y + 10, gfx::FontSize::Small, gfx::kFocus);
    SDL_RenderSetClipRect(app.gfx.renderer(), nullptr);
    if (loading) {
        app.gfx.text("Xbox library", status.x + 18, status.y + 57,
                     gfx::FontSize::Small, gfx::kTextDim);
        app.gfx.text("Please wait", status.x + 18, status.y + 101,
                     gfx::FontSize::Small, gfx::kText);
        return;
    }
    const char* compact_quality = app.settings.quality == 0 ? "720p"
                                  : app.settings.quality == 3 ? "1080p · Tizen"
                                  : app.settings.quality == 2 ? "1080p · HQ"
                                                               : "1080p";
    app.gfx.text(compact_quality, status.x + 18, status.y + 57,
                 gfx::FontSize::Small, gfx::kTextDim);
    app.gfx.text(std::to_string(app.visible.size()) + " games",
                 status.x + 18, status.y + 101, gfx::FontSize::Small,
                 gfx::kText);
}

std::string selected_game_meta(const App& app, const Game& game) {
    std::string meta = "Xbox Cloud Gaming · ";
    if (game.available_on_f2p) meta += "Owned / Free-to-play · ";
    meta += kQualityLabels[app.settings.quality];
    if (is_favorite(app, game.title_id)) meta += " · Favorite";
    return meta;
}

const HomeConsole& selected_console(const App& app) {
    return app.consoles[std::clamp(
        app.console_cursor, 0, static_cast<int>(app.consoles.size()) - 1)];
}

std::string console_label(const App& app) {
    if (app.consoles.empty()) return "Your Xbox";
    const HomeConsole& console = selected_console(app);
    return console.name.empty() ? "Your Xbox" : console.name;
}

#ifdef GNX_NATIVE_STREAM
std::string active_stream_region_label(const App& app) {
    std::string actual = app.engine->selected_region();
    if (actual.empty()) return selected_server_region_label(app);
    for (const ServerRegion& region : app.server_regions)
        if (normalized_server_region(region.name) ==
            normalized_server_region(actual))
            return server_region_label(region);
    return pretty_server_region(actual);
}

stream::QuickMenuState quick_menu_state(const App& app) {
    stream::QuickMenuState state;
    state.open = app.quick_menu_open;
    state.performance = app.settings.debug_hud != 0;
    state.pacing = app.settings.pacing;
    state.picture_profile = app.settings.picture_profile;
    state.brightness = app.settings.brightness;
    state.contrast = app.settings.contrast;
    state.saturation = app.settings.saturation;
    state.gamma = app.settings.gamma;
    state.sharpness = app.settings.sharpness;
    state.temperature = app.settings.temperature;
    return state;
}

void push_quick_menu_state(App& app, bool persist) {
    app.engine->set_quick_menu_state(quick_menu_state(app));
    if (persist) save_settings(app.settings);
}

// Returns true when the touch belongs to the quick menu. A tap outside closes
// an open panel but remains available to the independent Xbox touch target.
bool handle_quick_menu_touch(App& app, int x, int y) {
    if (point_in_rect(x, y, stream::kQuickToggleRect)) {
        app.quick_menu_open = !app.quick_menu_open;
        push_quick_menu_state(app, false);
        app.ui_sound.play(0.8f);
        return true;
    }
    if (!app.quick_menu_open) return false;

    if (!point_in_rect(x, y, stream::kQuickPanelRect)) {
        app.quick_menu_open = false;
        push_quick_menu_state(app, false);
        return false;
    }

    bool changed = false;
    if (point_in_rect(x, y,
                      stream::quick_row_rect(stream::QuickPerformance))) {
        app.settings.debug_hud = app.settings.debug_hud ? 0 : 1;
        changed = true;
    } else if (point_in_rect(x, y, stream::kQuickResetRect)) {
        app.settings.picture_profile = stream::PictureSignalPure;
        apply_profile_preset(app.settings, stream::PictureSignalPure);
        changed = true;
    } else {
        for (int row = stream::QuickPacing;
             row <= stream::QuickTemperature; ++row) {
            int direction = 0;
            if (point_in_rect(x, y, stream::quick_minus_rect(row)))
                direction = -1;
            else if (point_in_rect(x, y, stream::quick_plus_rect(row)))
                direction = 1;
            if (direction == 0) continue;

            if (row == stream::QuickPacing) {
                app.settings.pacing =
                    (app.settings.pacing + direction + 2) % 2;
            } else if (row == stream::QuickPictureProfile) {
                int next = app.settings.picture_profile;
                do {
                    next = (next + direction + 7) % 7;
                } while (next == stream::PictureOLEDAbyss && !is_switch_oled_handheld());
                app.settings.picture_profile = next;
                if (next != stream::PictureCustom) {
                    apply_profile_preset(app.settings, next);
                }
            } else if (row == stream::QuickBrightness) {
                app.settings.brightness = std::clamp(
                    app.settings.brightness + direction * 1, -20, 20);
                app.settings.picture_profile = stream::PictureCustom;
            } else if (row == stream::QuickContrast) {
                app.settings.contrast = std::clamp(
                    app.settings.contrast + direction * 1, 70, 130);
                app.settings.picture_profile = stream::PictureCustom;
            } else if (row == stream::QuickSaturation) {
                app.settings.saturation = std::clamp(
                    app.settings.saturation + direction * 1, 0, 150);
                app.settings.picture_profile = stream::PictureCustom;
            } else if (row == stream::QuickGamma) {
                app.settings.gamma = std::clamp(
                    app.settings.gamma + direction * 1, 50, 200);
                app.settings.picture_profile = stream::PictureCustom;
            } else if (row == stream::QuickSharpness) {
                app.settings.sharpness =
                    (app.settings.sharpness + direction + 4) % 4;
                app.settings.picture_profile = stream::PictureCustom;
            } else if (row == stream::QuickTemperature) {
                app.settings.temperature = std::clamp(
                    app.settings.temperature + direction * 1, -20, 20);
                app.settings.picture_profile = stream::PictureCustom;
            }
            changed = true;
            break;
        }
    }

    if (changed) {
        push_quick_menu_state(app, true);
        app.ui_sound.play(0.8f);
    }
    return true;
}
#endif

// Start the stream against the chosen target. launch_game must already be
// set (a game for xCloud; a pseudo-entry named after the console for home).
void launch_stream(App& app, bool home) {
    app.launching_home = home && !app.consoles.empty();
#ifdef GNX_NATIVE_STREAM
    // Release library artwork before the HTTP/WebRTC worker allocates session,
    // SDP and media buffers. The connecting screen lazily reloads only the one
    // selected cover, and the renderer handoff drops that again once video is
    // ready. This keeps page browsing from starving stream setup on Switch.
    app.covers->drop_textures();
    QualityTier tier = app.launching_home
                           ? (app.settings.console_quality == 1
                                  ? QualityTier::P1080
                                  : QualityTier::P720)
                           : static_cast<QualityTier>(app.settings.quality);
    const char* locale = kLanguageCodes[app.settings.language];
    app.engine->set_audio_gain(app.settings.volume);
    app.engine->set_pacing(
        static_cast<stream::VideoPacing>(app.settings.pacing));
    app.engine->set_force_region(app.settings.force_region != 0);
    const int kBitrateKbps[5] = {0, 7000, 14000, 20000, 30000};
    app.engine->set_max_bitrate_kbps(
        kBitrateKbps[std::clamp(app.settings.max_bitrate, 0, 4)]);
    app.quick_menu_open = false;
    push_quick_menu_state(app, false);
    if (app.launching_home)
        app.engine->start_home(selected_console(app).server_id, tier, locale);
    else
        app.engine->start(app.launch_game.title_id, tier, locale,
                          app.launch_game.uses_f2p_offering);
    app.stream_hint_until = SDL_GetTicks() + 8000;
    app.scene = Scene::Stream;
#endif
}

// Skeleton shades: each card one step darker, hinting at content loading in.
const gfx::Color kSkeleton[6] = {{22, 27, 36}, {20, 24, 33}, {18, 21, 29},
                                 {16, 19, 25}, {14, 17, 22}, {13, 15, 20}};

void draw_loading(App& app) {
    draw_header(app);
    draw_library_sidebar(app, false, true);
    app.gfx.fill(kLibraryPanel, {2, 14, 15, 214});
    app.gfx.frame(kLibraryPanel, {43, 104, 94, 155}, 2);
    SDL_Rect status_deck = {kGridX, 142, 1560, 104};
    app.gfx.fill(status_deck, {7, 31, 31, 232});
    app.gfx.frame(status_deck, {43, 104, 94, 185}, 1);
    app.gfx.text("SYNCING LIBRARY", status_deck.x + 24, status_deck.y + 34,
                 gfx::FontSize::Small, gfx::kFocus);
    for (int slot = 0; slot < kPageSize; ++slot) {
        int column = slot % kColumns;
        int row = slot / kColumns;
        app.gfx.fill({kGridX + column * (kCardW + kGapX),
                      kGridY + row * (kCardH + kGapY), kCardW, kCardH},
                     kSkeleton[slot % 6]);
    }
    app.gfx.spinner(status_deck.x + status_deck.w - 32,
                    status_deck.y + status_deck.h / 2, SDL_GetTicks());
    app.gfx.text_centered(app.status,
                           status_deck.x + status_deck.w / 2,
                           status_deck.y + 35,
                           gfx::FontSize::Note, gfx::kTextDim);
}

// Remove one complete UTF-8 code point from the end. This prevents a title
// ellipsis from leaving a broken continuation byte on screen.
void utf8_pop_back(std::string& value) {
    if (value.empty()) return;
    size_t start = value.size() - 1;
    while (start > 0 &&
           (static_cast<unsigned char>(value[start]) & 0xC0) == 0x80)
        --start;
    value.erase(start);
}

std::string ellipsize_library_title(App& app, std::string value,
                                    int max_width) {
    const std::string ellipsis = "…";
    if (app.gfx.text_width(value, gfx::FontSize::Small) <= max_width)
        return value;
    while (!value.empty() &&
           app.gfx.text_width(value + ellipsis, gfx::FontSize::Small) >
               max_width)
        utf8_pop_back(value);
    return value + ellipsis;
}

// Word-wrap a card title into at most two measured lines. The second line may
// use a UTF-8-safe ellipsis, while the selected-title marquee above the grid
// always exposes the complete unmodified title.
std::vector<std::string> wrap_library_title(App& app,
                                            const std::string& title,
                                            int max_width) {
    std::istringstream input(title);
    std::vector<std::string> words;
    std::string word;
    while (input >> word) words.push_back(word);
    if (words.empty()) return {title};

    std::string first;
    size_t index = 0;
    for (; index < words.size(); ++index) {
        std::string candidate = first.empty() ? words[index]
                                              : first + " " + words[index];
        if (app.gfx.text_width(candidate, gfx::FontSize::Small) <= max_width)
            first = std::move(candidate);
        else
            break;
    }
    if (first.empty()) {
        first = ellipsize_library_title(app, words.front(), max_width);
        index = 1;
    }
    if (index >= words.size()) return {first};

    std::string second;
    for (; index < words.size(); ++index) {
        if (!second.empty()) second += " ";
        second += words[index];
    }
    second = ellipsize_library_title(app, second, max_width);
    return {first, second};
}

void draw_library_title_marquee(App& app, const std::string& title,
                                 const SDL_Rect& box,
                                 gfx::FontSize size = gfx::FontSize::Small,
                                 gfx::Color color = gfx::kText) {
    SDL_Renderer* renderer = app.gfx.renderer();
    SDL_RenderSetClipRect(renderer, &box);
    const int width = app.gfx.text_width(title, size);
    if (width <= box.w) {
        app.gfx.text(title, box.x, box.y, size, color);
    } else {
        const int overflow = width - box.w;
        const int pause = 90;  // 1.5 s at the 60 Hz-oriented tick division.
        const int travel = std::max(1, overflow);
        const int phase = static_cast<int>((SDL_GetTicks() / 16) %
                                           (2 * pause + 2 * travel));
        int offset = 0;
        if (phase < pause)
            offset = 0;
        else if (phase < pause + travel)
            offset = phase - pause;
        else if (phase < 2 * pause + travel)
            offset = travel;
        else
            offset = 2 * pause + 2 * travel - phase;
        app.gfx.text(title, box.x - offset, box.y, size, color);
    }
    SDL_RenderSetClipRect(renderer, nullptr);
}

void draw_centered_clipped_text(App& app, const std::string& text,
                                const SDL_Rect& box, int y,
                                gfx::FontSize size, gfx::Color color) {
    SDL_Renderer* renderer = app.gfx.renderer();
    SDL_RenderSetClipRect(renderer, &box);
    app.gfx.text_centered(text, box.x + box.w / 2, y, size, color);
    SDL_RenderSetClipRect(renderer, nullptr);
}

// Professional full-bleed library card. Portrait covers are intentionally
// cropped only in this teaser grid; the detail screen still uses aspect-
// contain so the complete source image remains available. A dark title scrim
// keeps two measured lines readable without recreating the large black bars
// produced by letterboxing.
void draw_card(App& app, const Game& game, const SDL_Rect& card,
               bool focused) {
    SDL_Rect dst = card;
    if (focused) {
        float scale = 1.0f + 0.05f * library_focus_motion(app);
        dst = scale_about_center(card, scale);
    }

    SDL_Texture* cover = app.covers->get(game.title_id, game.box_art_url);
    if (cover)
        app.gfx.draw_texture_cover(cover, dst);
    else
        draw_cover_fallback(app, game, dst, gfx::FontSize::Title);

    // Two inexpensive translucent bands create a readable gradient transition
    // without blur textures or render targets. The opaque part is deliberately
    // shallow: the cover still occupies the whole visual card.
    SDL_Rect title_fade = {dst.x, dst.y + dst.h - 80, dst.w, 16};
    SDL_Rect title_area = {dst.x, dst.y + dst.h - 64, dst.w, 64};
    app.gfx.fill(title_fade, focused ? gfx::Color{2, 28, 27, 118}
                                     : gfx::Color{1, 10, 13, 104});
    app.gfx.fill(title_area, focused ? gfx::Color{3, 31, 29, 240}
                                     : gfx::Color{2, 14, 17, 232});
    app.gfx.fill({title_area.x + 10, title_area.y,
                  title_area.w - 20, 2},
                 focused ? gfx::kFocus : gfx::Color{43, 104, 94, 174});
    const std::string& label = game.name.empty() ? game.title_id : game.name;
    const std::vector<std::string> lines =
        wrap_library_title(app, label, std::max(1, dst.w - 24));
    int line_y = title_area.y + 6;
    for (size_t i = 0; i < lines.size() && i < 2; ++i) {
        app.gfx.text(lines[i], dst.x + 12, line_y, gfx::FontSize::Small,
                     gfx::kText);
        line_y += 25;
    }

    // Draw the edge after the artwork and scrim so it cannot disappear below
    // a full-bleed texture.
    app.gfx.frame(dst, focused ? gfx::kFocus : gfx::Color{43, 104, 94, 150},
                  focused ? 2 : 1);

    if (is_favorite(app, game.title_id)) {
        SDL_Rect badge = {dst.x + 8, dst.y + 8, 36, 36};
        app.gfx.fill(badge, gfx::kWarn);
        app.gfx.text_centered("★", badge.x + 18, badge.y + 2,
                              gfx::FontSize::Small, gfx::kBg);
    }

    if (focused) draw_focus_frames(app, dst);
}

void draw_selected_game_info(App& app, const Game& game) {
    float motion = std::clamp(library_focus_motion(app), 0.0f, 1.0f);
    int slide = static_cast<int>(12.0f * (1.0f - motion));
    // Layered bands create the left-to-right emerald hero gradient using the
    // native SDL renderer; no network hero artwork is required.
    app.gfx.fill(kLibraryHero, {3, 13, 17, 238});
    constexpr int kBands = 18;
    for (int band = 0; band < kBands; ++band) {
        int x = kLibraryHero.x + band * kLibraryHero.w / kBands;
        int next = kLibraryHero.x + (band + 1) * kLibraryHero.w / kBands;
        Uint8 green = static_cast<Uint8>(20 + band * 4);
        Uint8 alpha = static_cast<Uint8>(22 + band * 5);
        app.gfx.fill({x, kLibraryHero.y, next - x, kLibraryHero.h},
                     {5, green, 35, alpha});
    }
    app.gfx.frame(kLibraryHero, {35, 91, 82, 185}, 2);

    SDL_Rect cover_rect = {kLibraryHero.x + kLibraryHero.w - 340,
                           kLibraryHero.y + 24, 268, 402};
    SDL_Texture* cover = app.covers->get(game.title_id, game.box_art_url);
    if (cover)
        app.gfx.draw_texture_contain(cover, cover_rect);
    else
        draw_cover_fallback(app, game, cover_rect, gfx::FontSize::Huge);
    app.gfx.frame(cover_rect, {57, 224, 103, 120}, 2);

    const std::string& raw_title =
        game.name.empty() ? game.title_id : game.name;
    std::string title = raw_title;
    std::string meta = selected_game_meta(app, game);
    int text_x = kLibraryHero.x + 42;
    app.gfx.text("FEATURED", text_x, kLibraryHero.y + 190 + slide,
                 gfx::FontSize::Small, gfx::kFocus);
    app.gfx.text(title, text_x, kLibraryHero.y + 228 + slide,
                 gfx::FontSize::Title, gfx::kText);
    app.gfx.text(meta, text_x, kLibraryHero.y + 292 + slide,
                 gfx::FontSize::Small, gfx::kTextDim);

    SDL_Rect primary = {text_x, kLibraryHero.y + 340, 206, 68};
    app.gfx.fill(primary, gfx::kFocus);
    app.gfx.text_centered("A  Details", primary.x + primary.w / 2,
                          primary.y + 14, gfx::FontSize::Small, gfx::kBg);
    SDL_Rect secondary = {primary.x + primary.w + 18, primary.y, 238, 68};
    app.gfx.fill(secondary, gfx::kSurface);
    app.gfx.frame(secondary, gfx::kChipEdge, 2);
    app.gfx.text_centered(is_favorite(app, game.title_id)
                              ? "X  Remove favorite"
                              : "X  Add favorite",
                          secondary.x + secondary.w / 2, secondary.y + 14,
                          gfx::FontSize::Small, gfx::kText);
}

// Empty-state pattern (card 1l): big glyph box + title + instruction with
// the relevant button chip embedded in the line.
void draw_empty_state(App& app, const std::string& glyph, gfx::Color glyph_col,
                      const std::string& title, const std::string& pre,
                      const char* chip_key, const std::string& post) {
    const int content_center = kLibraryPanel.x + kLibraryPanel.w / 2;
    if (!glyph.empty()) {
        SDL_Rect box = {content_center - 60, 380, 120, 120};
        app.gfx.fill(box, gfx::kSurface);
        app.gfx.text_centered(glyph, box.x + 60, box.y + 28,
                              gfx::FontSize::Title, glyph_col);
    }
    SDL_Rect title_clip = {kLibraryPanel.x + 48, 522,
                           kLibraryPanel.w - 96, 64};
    SDL_RenderSetClipRect(app.gfx.renderer(), &title_clip);
    app.gfx.text_centered(title, content_center, 540, gfx::FontSize::Body,
                          gfx::kText);
    SDL_RenderSetClipRect(app.gfx.renderer(), nullptr);
    int pre_w = app.gfx.text_width(pre, gfx::FontSize::Note);
    int post_w = app.gfx.text_width(post, gfx::FontSize::Note);
    int cw = chip_key ? chip_width(app, chip_key) : 0;
    int total = pre_w + (chip_key ? 14 + cw + 14 : 0) + post_w;
    int x = content_center - total / 2;
    x += app.gfx.text(pre, x, 616, gfx::FontSize::Note, gfx::kTextDim);
    if (chip_key) {
        draw_chip(app, chip_key, x + 14, 612, false);
        x += 14 + cw + 14;
    }
    app.gfx.text(post, x, 616, gfx::FontSize::Note, gfx::kTextDim);
}

[[maybe_unused]] void draw_library_legacy(App& app) {
    // Row 1: identity (logo left, gamertag + source chip right). The source
    // chip only exists when a console is linked (card 1e visibility rule).
    draw_header(app);
    int right = gfx::kWidth - kMargin;
    if (!app.gamertag.empty()) {
        int gt_w = app.gfx.text_width(app.gamertag, gfx::FontSize::Small);
        app.gfx.text(app.gamertag, right - gt_w, 56, gfx::FontSize::Small,
                     gfx::kTextDim);
        right -= gt_w + 28;
    }
    if (!app.consoles.empty()) {
        std::string label =
            app.settings.source == 2
                ? console_label(app)
                : std::string("xCloud · ") +
                      kQualityLabels[app.settings.quality];
        int lw = app.gfx.text_width(label, gfx::FontSize::Small);
        SDL_Rect chip = {right - (lw + 64), 48, lw + 64, 44};
        app.gfx.fill(chip, gfx::kSurface);
        app.gfx.fill({chip.x + 20, chip.y + 16, 12, 12}, gfx::kFocus);
        app.gfx.text(label, chip.x + 44, chip.y + 6, gfx::FontSize::Small,
                     gfx::kText);
    }

    // A translucent stage groups navigation and covers while allowing the new
    // illustrated v0.4 background to remain visible around the edges.
    SDL_Rect stage = {120, 108, gfx::kWidth - 240, kFooterY - 132};
    app.gfx.fill(stage, {4, 10, 14, 96});
    app.gfx.frame(stage, {42, 74, 72, 150}, 2);

    // Consoles tab: one card per linked Xbox — pick one to stream.
    auto draw_console_cards = [&app]() {
        for (int i = 0; i < static_cast<int>(app.consoles.size()) && i < 3;
             ++i) {
            const HomeConsole& console = app.consoles[i];
            bool focused = i == app.console_cursor &&
                           !app.library_sidebar_focused;
            SDL_Rect card = {kGridX + i * (560 + 56), 300, 560, 380};
            app.gfx.fill(card, focused ? gfx::kSurfaceHi : gfx::kSurface);
            SDL_Rect icon = {card.x + 48, card.y + 56, 72, 72};
            app.gfx.fill(icon, focused ? gfx::kAccent : gfx::kChip);
            if (!focused) app.gfx.frame(icon, gfx::kChipEdge, 2);
            app.gfx.text_centered("⌂", icon.x + 36, icon.y + 14,
                                  gfx::FontSize::Body,
                                  focused ? gfx::kText : gfx::kTextDim);
            app.gfx.text(console.name.empty() ? "Your Xbox" : console.name,
                         card.x + 48, card.y + 164, gfx::FontSize::Body,
                         gfx::kText);
            app.gfx.text("Remote play from", card.x + 48, card.y + 220,
                         gfx::FontSize::Note, gfx::kTextDim);
            app.gfx.text(console.console_type, card.x + 48, card.y + 260,
                         gfx::FontSize::Note, gfx::kTextDim);
            bool on = console.power_state == "On";
            app.gfx.fill({card.x + 48, card.y + 322, 12, 12},
                         on ? gfx::kFocus : gfx::kWarn);
            app.gfx.text(console.power_state + " · LAN / internet",
                         card.x + 72, card.y + 312, gfx::FontSize::Small,
                         gfx::kTextDim);
            if (focused) draw_focus_frames(app, card);
        }
    };

    // Row 2: navigation — L/R chips hugging the tabs (the hint lives where it
    // acts), active tab kText + 5px accent underline, idle tabs kFaint.
    int tx = kGridX;
    draw_chip(app, "L", tx, 128, false);
    tx += chip_width(app, "L") + 44;
    int tabs = app.consoles.empty() ? kGameTabCount : kTabCount;
    for (int t = 0; t < tabs; ++t) {
        bool active = static_cast<int>(app.tab) == t;
        int w = app.gfx.text_width(kTabNames[t], gfx::FontSize::Body);
        if (active) {
            SDL_Rect pill = {tx - 18, 116, w + 36, 60};
            app.gfx.fill(pill, {16, 124, 16, 92});
            app.gfx.frame(pill, {57, 224, 103, 150}, 2);
        }
        app.gfx.text(kTabNames[t], tx, 124, gfx::FontSize::Body,
                     active ? gfx::kText : gfx::kFaint);
        tx += w + 44;
    }
    draw_chip(app, "R", tx, 128, false);

    if (app.tab == LibraryTab::Consoles) {
        std::string info =
            std::to_string(app.consoles.size()) +
            (app.consoles.size() == 1 ? " console" : " consoles");
        app.gfx.text(info,
                     gfx::kWidth - kGridX -
                         app.gfx.text_width(info, gfx::FontSize::Small),
                     138, gfx::FontSize::Small, gfx::kFaint);
        draw_console_cards();
        draw_hints(app, {{"A", "Connect", true},
                         {"L R", "Tabs"},
                         {"ZR", "Refresh"},
                         {"ZL", "Settings"},
                         {"+", "Exit"}});
        return;
    }

    std::string info = std::to_string(app.visible.size()) + " games";
    if (!app.query.empty()) info += "  ·  \"" + app.query + "\"";
    app.gfx.text(info,
                 gfx::kWidth - kGridX -
                     app.gfx.text_width(info, gfx::FontSize::Small),
                 138, gfx::FontSize::Small, gfx::kFaint);

    if (app.visible.empty()) {
        if (!app.query.empty())
            draw_empty_state(app, "", gfx::kText,
                             "Nothing found for \"" + app.query + "\"",
                             "Press", "Y", "to search again");
        else if (app.tab == LibraryTab::Favorites)
            draw_empty_state(app, "★", gfx::kWarn, "No favorites yet",
                             "Press", "X", "on any game to pin it here");
        else if (app.tab == LibraryTab::History)
            draw_empty_state(app, "…", gfx::kTextDim, "Nothing played yet",
                             "Games you launch appear here", nullptr, "");
        else if (app.tab == LibraryTab::OwnedFree)
            draw_empty_state(app, "", gfx::kFocus,
                             "No owned or free games available",
                             "Press", "ZR", "to refresh both Xbox catalogs");
        else
            draw_empty_state(app, "", gfx::kText,
                             "No games available for this account",
                             "Press", "ZR", "to refresh your library");
    } else {
        draw_selected_game_info(
            app, app.games[app.visible[std::clamp(
                     app.cursor, 0, static_cast<int>(app.visible.size()) - 1)]]);
    }

    // Draw the focused card last so its scale/glow overlaps neighbours.
    int first_row = std::max(0, app.cursor / kColumns - (kRowsVisible - 1));
    int focused_slot = -1;
    for (int slot = 0; slot < kColumns * (kRowsVisible + 1); ++slot) {
        int index = first_row * kColumns + slot;
        if (index >= static_cast<int>(app.visible.size())) break;
        int column = slot % kColumns;
        int row = slot / kColumns;
        SDL_Rect card = {kGridX + column * (kCardW + kGapX),
                         kGridY + row * (kCardH + kGapY), kCardW, kCardH};
        if (card.y + 40 > kFooterY) break;
        if (index == app.cursor) {
            focused_slot = slot;
            continue;
        }
        draw_card(app, app.games[app.visible[index]], card, false);
    }
    if (focused_slot >= 0) {
        int column = focused_slot % kColumns;
        int row = focused_slot / kColumns;
        SDL_Rect card = {kGridX + column * (kCardW + kGapX),
                         kGridY + row * (kCardH + kGapY), kCardW, kCardH};
        draw_card(app, app.games[app.visible[app.cursor]], card, true);
    }

    draw_hints(app, {{"A", "Details", true},
                     {"X", "Favorite"},
                     {"Y", "Search"},
                     {"ZR", "Refresh"},
                     {"ZL", "Settings"},
                     {"+", "Exit"}});
}

// v1.0.3 library: one persistent navigation hierarchy on the left, with the
// content deck and its three grid rows on the right. Shoulder shortcuts remain
// available, but the rail is also reachable by pressing Left from column zero.
void draw_library(App& app) {
    draw_header(app);
    app.gfx.fill(kLibraryPanel, {2, 14, 15, 214});
    app.gfx.frame(kLibraryPanel, {43, 104, 94, 155}, 2);
    draw_library_sidebar(app);

    if (app.tab == LibraryTab::Consoles) {
        for (int i = 0; i < static_cast<int>(app.consoles.size()) && i < 3;
             ++i) {
            const HomeConsole& console = app.consoles[i];
            bool focused = i == app.console_cursor &&
                           !app.library_sidebar_focused;
            SDL_Rect card = {kConsoleGridX + i * (kConsoleCardW + kConsoleGapX),
                             kConsoleGridY, kConsoleCardW, kConsoleCardH};
            app.gfx.fill(card, focused ? gfx::kSurfaceHi : gfx::kSurface);
            app.gfx.frame(card, focused ? gfx::kFocus
                                        : gfx::Color{43, 104, 94, 145},
                          focused ? 3 : 1);
            SDL_Rect icon = {card.x + 40, card.y + 46, 88, 88};
            app.gfx.fill(icon, {5, 34, 33, 238});
            app.gfx.frame(icon, focused ? gfx::kFocus : gfx::kChipEdge, 2);
            app.gfx.draw_brand_icon({icon.x + 10, icon.y + 10,
                                     icon.w - 20, icon.h - 20});
            const std::string console_name =
                console.name.empty() ? "Your Xbox" : console.name;
            draw_library_title_marquee(
                app, console_name,
                {card.x + 40, card.y + 164, card.w - 80, 48},
                gfx::FontSize::Body);
            app.gfx.text("Remote play from", card.x + 48, card.y + 220,
                         gfx::FontSize::Note, gfx::kTextDim);
            app.gfx.text(console.console_type, card.x + 48, card.y + 260,
                         gfx::FontSize::Note, gfx::kTextDim);
            bool on = console.power_state == "On";
            app.gfx.fill({card.x + 48, card.y + 322, 12, 12},
                         on ? gfx::kFocus : gfx::kWarn);
            app.gfx.text(console.power_state + " · LAN / internet",
                         card.x + 72, card.y + 312,
                         gfx::FontSize::Small, gfx::kTextDim);
            if (focused) draw_focus_frames(app, card);
        }
        draw_hints(app, {{"A", "Connect", true},
                         {"L R", "Tabs"},
                         {"ZR", "Refresh"},
                         {"ZL", "Settings"},
                         {"+", "Exit"}});
        return;
    }

    if (app.visible.empty()) {
        if (!app.query.empty())
            draw_empty_state(app, "", gfx::kText,
                             "Nothing found for \"" + app.query + "\"",
                             "Press", "Y", "to search again");
        else if (app.tab == LibraryTab::Favorites)
            draw_empty_state(app, "★", gfx::kWarn, "No favorites yet",
                             "Press", "X", "on any game to pin it here");
        else if (app.tab == LibraryTab::History)
            draw_empty_state(app, "…", gfx::kTextDim,
                             "Nothing played yet",
                             "Games you launch appear here", nullptr, "");
        else if (app.tab == LibraryTab::OwnedFree)
            draw_empty_state(app, "", gfx::kFocus,
                             "No owned or free games available",
                             "Press", "ZR", "to refresh both Xbox catalogs");
        else
            draw_empty_state(app, "", gfx::kText,
                             "No games available for this account",
                             "Press", "ZR", "to refresh your library");
        draw_hints(app, {{"Y", "Search", true},
                         {"ZR", "Refresh"},
                         {"ZL", "Settings"},
                         {"+", "Exit"}});
        return;
    }

    app.cursor = std::clamp(app.cursor, 0,
                            static_cast<int>(app.visible.size()) - 1);
    const Game& selected = app.games[app.visible[app.cursor]];
    SDL_Rect command_deck = {kGridX, 142, 1560, 104};
    app.gfx.fill(command_deck, {7, 31, 31, 232});
    app.gfx.frame(command_deck, {43, 104, 94, 185}, 1);

    const char* section = app.tab == LibraryTab::Favorites
                              ? "Favorites"
                           : app.tab == LibraryTab::History
                               ? "Recently played"
                           : app.tab == LibraryTab::OwnedFree
                               ? "Owned + Free"
                               : "All games";
    app.gfx.text(section, command_deck.x + 24, command_deck.y + 17,
                 gfx::FontSize::Small,
                 gfx::kFocus);
    app.gfx.fill({command_deck.x + 214, command_deck.y + 14, 2,
                  command_deck.h - 20},
                 {43, 104, 94, 190});
    int page_start = (app.cursor / kPageSize) * kPageSize;
    int page_number = page_start / kPageSize + 1;
    int page_count =
        (static_cast<int>(app.visible.size()) + kPageSize - 1) / kPageSize;
    std::string count = std::to_string(app.visible.size()) + " games  ·  " +
                        std::to_string(page_number) + "/" +
                        std::to_string(page_count);
    app.gfx.text(count,
                 command_deck.x + command_deck.w - 24 -
                     app.gfx.text_width(count, gfx::FontSize::Small),
                 command_deck.y + 58, gfx::FontSize::Small, gfx::kFocus);

    const std::string& selected_label =
        selected.name.empty() ? selected.title_id : selected.name;
    SDL_Rect selected_title_box = {command_deck.x + 244,
                                   command_deck.y + 12, 650, 32};
    draw_library_title_marquee(app, selected_label, selected_title_box);
    draw_library_title_marquee(
        app, selected_game_meta(app, selected),
        {command_deck.x + 244, command_deck.y + 55, 650, 30},
        gfx::FontSize::Small, gfx::kTextDim);
    app.gfx.text("A  DETAILS", command_deck.x + 930,
                 command_deck.y + 17, gfx::FontSize::Small,
                 gfx::kText);
    app.gfx.text(is_favorite(app, selected.title_id) ? "X  PINNED"
                                                      : "X  FAVORITE",
                 command_deck.x + 1120, command_deck.y + 17,
                 gfx::FontSize::Small,
                 is_favorite(app, selected.title_id) ? gfx::kWarn
                                                      : gfx::kTextDim);

    int focused_slot = app.cursor - page_start;
    for (int slot = 0; slot < kPageSize; ++slot) {
        int index = page_start + slot;
        if (index >= static_cast<int>(app.visible.size())) break;
        if (slot == focused_slot) continue;
        int column = slot % kColumns;
        int row = slot / kColumns;
        SDL_Rect card = {kGridX + column * (kCardW + kGapX),
                         kGridY + row * (kCardH + kGapY), kCardW, kCardH};
        draw_card(app, app.games[app.visible[index]], card, false);
    }
    int focused_column = focused_slot % kColumns;
    int focused_row = focused_slot / kColumns;
    SDL_Rect focused_card = {kGridX + focused_column * (kCardW + kGapX),
                             kGridY + focused_row * (kCardH + kGapY),
                             kCardW, kCardH};
    // Exactly one control owns the focus glow. While the rail is active the
    // selected game remains the current item, but its card is visually idle.
    draw_card(app, selected, focused_card, !app.library_sidebar_focused);

    draw_hints(app, {{"A", "Details", true},
                     {"X", "Favorite"},
                     {"Y", "Search"},
                     {"ZR", "Refresh"},
                     {"ZL", "Settings"},
                     {"+", "Exit"}});
}

// Game detail: big cover left, stable local metadata + action buttons right.
// Play is focused on entry, so A-A launches as fast as before.
void draw_detail(App& app) {
    if (app.detail_index < 0 ||
        app.detail_index >= static_cast<int>(app.games.size()))
        return;
    const Game& game = app.games[app.detail_index];

    draw_header(app);
    SDL_Rect stage = {kMargin, 132, gfx::kWidth - 2 * kMargin, 790};
    app.gfx.fill(stage, {2, 14, 15, 220});
    app.gfx.frame(stage, {43, 104, 94, 170}, 2);

    SDL_Rect cover_rect = {1240, 180, 500, 660};
    app.gfx.fill(cover_rect, gfx::kSurface);
    SDL_Texture* cover = app.covers->get(game.title_id, game.box_art_url);
    if (cover)
        app.gfx.draw_texture_contain(cover, cover_rect);
    else
        draw_cover_fallback(app, game, cover_rect, gfx::FontSize::Huge);
    app.gfx.frame(cover_rect, {45, 244, 165, 145}, 2);
    bool fav = is_favorite(app, game.title_id);
    if (fav) {
        SDL_Rect badge = {cover_rect.x + 12, cover_rect.y + 12, 56, 56};
        app.gfx.fill(badge, gfx::kWarn);
        app.gfx.text_centered("★", badge.x + 28, badge.y + 10,
                              gfx::FontSize::Body, gfx::kBg);
    }

    int rx = 120;
    const std::string& title = game.name.empty() ? game.title_id : game.name;
    app.gfx.text("GAME DETAILS", rx, 170, gfx::FontSize::Small,
                 gfx::kFocus);
    draw_library_title_marquee(app, title, {rx, 210, 1040, 70},
                               gfx::FontSize::Title);

    // Meta chips: kSurface pills with XS text.
    int cx2 = rx;
    auto meta_chip = [&](const std::string& label, gfx::Color color) {
        int w = app.gfx.text_width(label, gfx::FontSize::Small) + 36;
        app.gfx.fill({cx2, 310, w, 44}, gfx::kSurface);
        app.gfx.frame({cx2, 310, w, 44}, {43, 104, 94, 145}, 1);
        app.gfx.text(label, cx2 + 18, 316, gfx::FontSize::Small, color);
        cx2 += w + 20;
    };
    meta_chip(game.available_on_f2p ? "OWNED / FREE" : "GAME PASS",
              gfx::kTextDim);
    meta_chip(kQualityLabels[app.settings.quality], gfx::kTextDim);
    if (fav) meta_chip("★ Favorite", gfx::kWarn);

    // Action buttons, 640 wide. Buttons don't scale on focus — only
    // border+glow (and the primary keeps its accent fill). "Play on…" is
    // drawn only with a linked console (card 1f visibility rule).
    const char* fav_label = fav ? "★ Remove favorite" : "★ Add favorite";
    SDL_Rect play = {rx, 430, 860, 94};
    app.gfx.fill(play, gfx::kAccent);
    app.gfx.frame(play, {45, 244, 165, 210}, 2);
    app.gfx.text_centered("Play now", play.x + play.w / 2, play.y + 23,
                          gfx::FontSize::Body, gfx::kText);
    SDL_Rect favbtn = {rx, 548, 860, 94};
    app.gfx.fill(favbtn, gfx::kSurface);
    app.gfx.frame(favbtn, {43, 104, 94, 165}, 2);
    app.gfx.text_centered(fav_label, favbtn.x + favbtn.w / 2, favbtn.y + 23,
                          gfx::FontSize::Body, gfx::kText);
    SDL_Rect source = {rx, 666, 860, 94};
    if (!app.consoles.empty()) {
        app.gfx.fill(source, gfx::kSurface);
        app.gfx.frame(source, {43, 104, 94, 165}, 2);
        app.gfx.text("Play on…", source.x + 44, source.y + 24,
                     gfx::FontSize::Body, gfx::kText);
        std::string target = app.settings.source == 2   ? console_label(app)
                             : app.settings.source == 1 ? "xCloud"
                                                        : "Ask every time";
        app.gfx.text(target,
                     source.x + source.w - 44 -
                         app.gfx.text_width(target, gfx::FontSize::Body),
                     source.y + 23, gfx::FontSize::Body, gfx::kTextDim);
    }
    SDL_Rect focused = app.detail_cursor == 0   ? play
                       : app.detail_cursor == 1 ? favbtn
                                                : source;
    draw_focus_frames(app, focused);

    app.gfx.text("Language and stream preferences are available in Settings",
                 rx, app.consoles.empty() ? 690 : 788, gfx::FontSize::Small,
                 gfx::kFaint);

    draw_hints(app, {{"A", "Select", true}, {"B", "Back"}});
}

// Source picker (card 1k): shown on Play when a console is linked and no
// default source was fixed. Hold A on a card to make it the default and
// skip this screen from then on.
void draw_source_picker(App& app) {
    const std::string& game =
        app.launch_game.name.empty() ? app.launch_game.title_id
                                     : app.launch_game.name;
    draw_header(app);
    app.gfx.text_centered("CHOOSE A PLAY SOURCE", gfx::kWidth / 2, 142,
                          gfx::FontSize::Small, gfx::kFocus);
    draw_library_title_marquee(app, "Play " + game + " on…",
                               {200, 182, gfx::kWidth - 400, 66},
                               gfx::FontSize::Title);

    int total = 560 * 2 + 56;
    int x0 = (gfx::kWidth - total) / 2;
    for (int i = 0; i < 2; ++i) {
        bool focused = app.pick_cursor == i;
        SDL_Rect card = {x0 + i * (560 + 56), 320, 560, 420};
        app.gfx.fill(card, focused ? gfx::kSurfaceHi : gfx::kSurface);
        app.gfx.frame(card, {43, 104, 94, 160}, 2);

        SDL_Rect icon = {card.x + 48, card.y + 56, 72, 72};
        app.gfx.fill(icon, i == 0 ? gfx::kAccent : gfx::kChip);
        if (i == 1) app.gfx.frame(icon, gfx::kChipEdge, 2);
        app.gfx.text_centered(i == 0 ? "☁" : "⌂", icon.x + 36, icon.y + 14,
                              gfx::FontSize::Body,
                              i == 0 ? gfx::kText : gfx::kTextDim);

        if (i == 0) {
            app.gfx.text("Cloud Play", card.x + 48, card.y + 164,
                         gfx::FontSize::Body, gfx::kText);
            app.gfx.text("Play in the cloud.", card.x + 48, card.y + 224,
                         gfx::FontSize::Note, gfx::kTextDim);
            app.gfx.text("No console needed.", card.x + 48, card.y + 264,
                         gfx::FontSize::Note, gfx::kTextDim);
            app.gfx.fill({card.x + 48, card.y + 352, 12, 12}, gfx::kFocus);
            app.gfx.text(std::string("Ready · ") +
                             kQualityLabels[app.settings.quality],
                         card.x + 72, card.y + 342, gfx::FontSize::Small,
                         gfx::kTextDim);
        } else {
            const HomeConsole& console = selected_console(app);
            app.gfx.text(console_label(app), card.x + 48, card.y + 164,
                         gfx::FontSize::Body, gfx::kText);
            app.gfx.text("Remote play from", card.x + 48, card.y + 224,
                         gfx::FontSize::Note, gfx::kTextDim);
            app.gfx.text(console.console_type, card.x + 48, card.y + 264,
                         gfx::FontSize::Note, gfx::kTextDim);
            bool on = console.power_state == "On";
            app.gfx.fill({card.x + 48, card.y + 352, 12, 12},
                         on ? gfx::kFocus : gfx::kWarn);
            app.gfx.text(console.power_state + " · LAN / internet",
                         card.x + 72, card.y + 342, gfx::FontSize::Small,
                         gfx::kTextDim);
        }
        if (focused) draw_focus_frames(app, card);
    }

    app.gfx.text_centered(
        "Hold A on either option to make it the default and skip this screen",
        gfx::kWidth / 2, 880, gfx::FontSize::Small, gfx::kFaint);
    draw_hints(app, {{"A", "Play", true}, {"B", "Back"}});
}

// Account picker: the known accounts plus an "Add account" row. Reached from
// Settings; A switches to the highlighted account, B goes back.
void draw_accounts(App& app) {
    app.gfx.text("Accounts", kMargin, 48, gfx::FontSize::Title, gfx::kText);

    int add_row = static_cast<int>(g_accounts.size());
    int count = add_row + 1;
    int pitch = count <= 6 ? 108 : 92;
    for (int i = 0; i < count; ++i) {
        SDL_Rect row = {120, 170 + i * pitch, gfx::kWidth - 240, 96};
        bool focused = i == app.accounts_cursor;
        app.gfx.fill(row, focused ? gfx::kSurfaceHi : gfx::kSurface);
        if (focused) {
            app.gfx.fill({row.x, row.y, 10, row.h}, gfx::kFocus);
            app.gfx.frame(row, gfx::kFocus, 4);
        }
        std::string title;
        std::string value;
        if (i == add_row) {
            title = "Add account";
            value = "Sign in with another account";
        } else {
            const Account& account = g_accounts[i];
            title = account.gamertag.empty()
                        ? "Account " + std::to_string(i + 1)
                        : account.gamertag;
            if (account.id == g_active_account) {
                value = "Active";
            } else if (focused && app.remove_armed) {
                value = "Press X again to remove";
            } else {
                value = account_has_login(account.id) ? "Signed in"
                                                      : "Not signed in";
            }
        }
        app.gfx.text(title, row.x + 68, row.y + 26, gfx::FontSize::Body,
                     gfx::kText);
        int vw = app.gfx.text_width(value, gfx::FontSize::Body);
        app.gfx.text(value, row.x + row.w - 44 - vw, row.y + 26,
                     gfx::FontSize::Body,
                     focused && app.remove_armed && i < add_row ? gfx::kError
                     : i < add_row && g_accounts[i].id == g_active_account
                         ? gfx::kAccent
                         : gfx::kTextDim);
    }
    // X only applies to the accounts you are not currently using; the active
    // one leaves through Settings -> Sign out.
    bool can_remove = app.accounts_cursor < add_row &&
                      g_accounts[app.accounts_cursor].id != g_active_account;
    std::vector<Hint> hints = {
        {"A", app.accounts_cursor == add_row ? "Add" : "Switch"}};
    if (can_remove)
        hints.push_back({"X", app.remove_armed ? "Confirm remove" : "Remove"});
    hints.push_back({"B", "Back"});
    draw_hints(app, hints);
}

void draw_settings(App& app) {
    // A stream login may have refreshed Xbox's live datacenter list while the
    // menu was not visible. Import it once and persist it for the next launch.
    sync_server_regions(app);
    draw_header(app);
    app.gfx.fill(kLibraryPanel, {2, 14, 15, 214});
    app.gfx.frame(kLibraryPanel, {43, 104, 94, 155}, 2);
    draw_library_sidebar(app, true);
    app.gfx.text("SETTINGS", kGridX, 136, gfx::FontSize::Small, gfx::kFocus);
    auto signed_value = [](int value) {
        return std::string(value > 0 ? "+" : "") + std::to_string(value);
    };

    struct Row {
        const char* title;
        std::string value;
    };
    const char* kForceRegionLabels[2] = {"Off (Allow Fallback)", "On (Strict Region Only)"};
    const char* kBitrateLabels[5] = {"Auto", "7 Mbps (Low)", "14 Mbps (Medium)", "20 Mbps (High)", "30 Mbps (Ultra HQ)"};

    std::vector<Row> rows = {
        {"Server region", selected_server_region_label(app)},
        {"Region bypass", kRegionLabels[app.settings.region]},
        {"Force region", kForceRegionLabels[app.settings.force_region]},
        {"Max bitrate", kBitrateLabels[app.settings.max_bitrate]},
        {"Stream quality", kQualityLabels[app.settings.quality]},
        {"Console quality",
         kConsoleQualityLabels[app.settings.console_quality]},
        {"Button layout", kMappingLabels[app.settings.mapping]},
        {"Vibration", kVibrationLabels[app.settings.vibration]},
        {"Game language", kLanguageLabels[app.settings.language]},
        {"Volume",
         std::to_string(static_cast<int>(app.settings.volume * 100 + 0.5f)) + "%"},
        {"Pacing", kPacingLabels[app.settings.pacing]},
        {"Brightness", signed_value(app.settings.brightness)},
        {"Contrast", std::to_string(app.settings.contrast) + "%"},
        {"Saturation", std::to_string(app.settings.saturation) + "%"},
        {"Gamma", [&app] {
             char value[16];
             std::snprintf(value, sizeof(value), "%.2f",
                           app.settings.gamma / 100.0f);
             return std::string(value);
        }()},
        {"Sharpness", kSharpnessLabels[app.settings.sharpness]},
    };
    if (!app.consoles.empty())
        rows.push_back({"Preferred source",
                        app.settings.source == 2   ? console_label(app)
                        : app.settings.source == 1 ? "xCloud"
                                                   : "Ask every time"});
    rows.push_back({"Debug HUD", app.settings.debug_hud ? "On" : "Off"});
    int accounts_row = static_cast<int>(rows.size());
    rows.push_back({"Accounts",
                    std::to_string(g_accounts.size()) +
                        (g_accounts.size() == 1 ? " account" : " accounts")});
    // Sign out lives here rather than on a library shoulder button so a stray
    // press can never log the account out; it also takes a second A to confirm.
    int signout_row = static_cast<int>(rows.size());
    rows.push_back({"Sign out", app.signout_armed
                                    ? "Press A again to confirm"
                                    : app.gamertag});
    // The list must clear the note box at y=820, which fits 8 rows at the
    // tightened 78/70 pitch. Picture controls and account actions overflow
    // that, so an 8-row window slides only when the cursor crosses its edge.
    constexpr int kVisibleRows = 8;
    int shown = std::min(static_cast<int>(rows.size()), kVisibleRows);
    int first = std::clamp(app.settings_cursor - (kVisibleRows - 1), 0,
                           static_cast<int>(rows.size()) - shown);
    int pitch = shown <= 6 ? 108 : shown <= 7 ? 92 : 78;
    int row_h = shown <= 7 ? 96 : 70;
    if (first > 0)
        app.gfx.text("· · ·", kGridX + 1560 / 2 - 24, 140,
                     gfx::FontSize::Small, gfx::kFaint);
    if (first + shown < static_cast<int>(rows.size()))
        app.gfx.text("· · ·", kGridX + 1560 / 2 - 24,
                     170 + (shown - 1) * pitch + row_h + 4,
                     gfx::FontSize::Small, gfx::kFaint);
    for (int i = first; i < first + shown; ++i) {
        SDL_Rect row = {kGridX, 170 + (i - first) * pitch, 1560,
                        row_h};
        bool focused = i == app.settings_cursor;
        // Row-focus variant (card 1g): wide elements don't scale — surface
        // lift + 10px side bar + 4px border + one glow frame instead.
        app.gfx.fill(row, focused ? gfx::kSurfaceHi : gfx::kSurface);
        if (focused) {
            app.gfx.fill({row.x, row.y, 10, row.h}, gfx::kFocus);
            app.gfx.frame(row, gfx::kFocus, 4);
        } else {
            app.gfx.frame(row, gfx::kChipEdge, 2);
        }
        app.gfx.text(rows[i].title, row.x + 36, row.y + 26,
                     gfx::FontSize::Body,
                     focused ? gfx::kText : gfx::kTextDim);
        int vw = app.gfx.text_width(rows[i].value, gfx::FontSize::Body);
        if (i == signout_row || i == accounts_row) {
            // Action rows: no ‹ › carets (A opens/confirms them), and the
            // armed sign-out reads as danger.
            app.gfx.text(rows[i].value, row.x + row.w - 44 - vw, row.y + 26,
                         gfx::FontSize::Body,
                         i == signout_row && app.signout_armed ? gfx::kError
                         : focused                            ? gfx::kText
                                                              : gfx::kTextDim);
        } else if (focused) {
            int vx = row.x + row.w - 44 - vw;
            app.gfx.text("‹", vx - 56, row.y + 26, gfx::FontSize::Body,
                         gfx::kTextDim);
            app.gfx.text(rows[i].value, vx, row.y + 26, gfx::FontSize::Body,
                         gfx::kFocus);
            app.gfx.text("›", row.x + row.w - 44 + 20, row.y + 26,
                         gfx::FontSize::Body, gfx::kTextDim);
        } else {
            app.gfx.text(rows[i].value, row.x + row.w - 44 - vw, row.y + 26,
                         gfx::FontSize::Body, gfx::kAccent);
        }
    }

    // Contextual note: a fixed structured box (fill kBar + frame + accent
    // side bar), swapping content with the focused row. The bar turns kWarn
    // while Region bypass is active.
    const char* line1;
    const char* line2;
    if (app.settings_cursor == signout_row) {
        line1 = "Signs this Switch out of your Microsoft account and clears";
        line2 = "the saved sign-in. Cloud saves and games are not affected.";
    } else if (app.settings_cursor == accounts_row) {
        line1 = "Share the console: each account keeps its own sign-in,";
        line2 = "library and favorites. Press A to switch or add one.";
    } else if (app.settings_cursor == accounts_row - 1) {
        line1 = "On-screen overlay with live stream stats (resolution, FPS,";
        line2 = "bitrate, loss). A debug tool -- turn it off for clean playback.";
    } else switch (app.settings_cursor) {
        case 0:
            line1 = "Chooses the xCloud datacenter. Auto follows Xbox; a fixed";
            line2 = "region can avoid a busy queue but may add network latency.";
            break;
        case 1:
            line1 = "Region bypass spoofs your location to Xbox to reach";
            line2 = "xCloud from an unsupported country. Use at your own risk.";
            break;
        case 2:
            line1 = "Forces xCloud to use ONLY your chosen region with NO fallback.";
            line2 = "Prevents Xbox from moving the session to any other datacenter.";
            break;
        case 3:
            line1 = "Controls maximum WebRTC video stream bitrate.";
            line2 = "Auto adapts; higher values (20-30 Mbps) improve image sharpness.";
            break;
        case 4:
            if (app.settings.quality == 3) {
                line1 = "Experimental TV/Tizen pool. It may provide high bitrate";
                line2 = "but can queue longer; use HQ Windows if that happens.";
            } else {
                line1 = "Announces your device tier to Xbox. HQ Windows gives";
                line2 = "the best 1080p pool; 720p uses less bandwidth.";
            }
            break;
        case 5:
            if (app.settings.console_quality == 1) {
                line1 = "Experimental 1080p Remote Play request. Compatibility";
                line2 = "depends on Xbox; no video after 15s retries at 720p.";
            } else {
                line1 = "Stable 720p Remote Play profile for your own Xbox.";
                line2 = "Choose 1080p only for beta testing on a strong link.";
            }
            break;
        case 6:
            line1 = "Positional keeps the Switch layout under your thumbs;";
            line2 = "match labels follows the printed A/B/X/Y letters.";
            break;
        case 7:
            line1 = "Rumble intensity for the game's vibration effects.";
            line2 = "High still leaves headroom to avoid the HD-rumble hum.";
            break;
        case 8:
            line1 = "Sets the streamed console's language for games without";
            line2 = "an in-game language menu. Takes effect on next launch.";
            break;
        case 9:
            line1 = "Output volume for streamed audio - raise it if the stream";
            line2 = "sounds quiet even with the console at full volume.";
            break;
        case 10:
            if (app.settings.pacing == 0) {
                line1 = "Steady prioritizes latency and repeats the newest frame";
                line2 = "on a local 60 Hz clock when the network arrives late.";
            } else if (app.settings.pacing == 1) {
                line1 = "Smooth holds one source frame to absorb network jitter.";
                line2 = "It is steadier, with about one frame of extra lag.";
            } else {
                line1 = "Motion is Luma 50% midpoint frame generation (30->60 Hz).";
                line2 = "Smooth 60 Hz motion blending with no green flashing.";
            }
            break;
        case 11:
            line1 = "Adds or removes light after video color conversion.";
            line2 = "Small changes work best; high values can clip highlights.";
            break;
        case 12:
            line1 = "Expands or compresses the difference between dark and";
            line2 = "bright areas. 100% preserves the source image.";
            break;
        case 13:
            line1 = "Controls color intensity. 100% is neutral; 0% is";
            line2 = "grayscale, while higher values make colors stronger.";
            break;
        case 14:
            line1 = "Adjusts midtones without moving the darkest blacks or";
            line2 = "brightest whites. 1.00 is neutral; higher is brighter.";
            break;
        case 15:
            line1 = "Sharpens the streamed image, which is a touch soft at";
            line2 = "cloud bitrates. Low is subtle; High can ring on edges.";
            break;
        case 16:
            line1 = "Where Play launches games: xCloud (cloud servers) or";
            line2 = "Remote Play from your Xbox, including away from home.";
            break;
        default:
            line1 = "Higher quality needs a stronger connection — 5 GHz";
            line2 = "Wi-Fi or docked LAN is recommended for high bitrate.";
            break;
    }
    SDL_Rect note = {kGridX, 820, 1560, 120};
    app.gfx.fill(note, gfx::kBar);
    app.gfx.frame(note, gfx::kChip, 2);
    app.gfx.fill({note.x + 28, note.y + 28, 8, 64},
                 app.signout_armed && app.settings_cursor == signout_row
                     ? gfx::kError
                 : app.settings.region != 0 ? gfx::kWarn
                                            : gfx::kAccent);
    app.gfx.text(line1, note.x + 64, note.y + 20, gfx::FontSize::Note,
                 gfx::kTextDim);
    app.gfx.text(line2, note.x + 64, note.y + 60, gfx::FontSize::Note,
                 gfx::kTextDim);

    if (!app.last_exit_step.empty())
        app.gfx.text("debug · last exit reached: " + app.last_exit_step,
                     kGridX, kFooterY - 36, gfx::FontSize::Small,
                     gfx::kFaint);
    if (app.settings_cursor == signout_row)
        draw_hints(app, {{"A", app.signout_armed ? "Confirm sign out"
                                                 : "Sign out", true},
                         {"B", "Back"}});
    else
        draw_hints(app, {{"◀ ▶", "Change"}, {"B", "Back"}});
}

#ifdef GNX_NATIVE_STREAM
// mapping 0: positional (Switch east button -> Xbox east button).
// mapping 1: match labels (Switch A -> Xbox A).
xcloud::GamepadFrame read_gamepad(SDL_Joystick* joystick, int mapping,
                                  bool force_nexus = false) {
    xcloud::GamepadFrame frame;
    auto button = [&](int index) {
        return SDL_JoystickGetButton(joystick, index) != 0;
    };
    if (mapping == 0) {
        frame.b = button(kBtnA);       // Switch A (east)  -> Xbox B
        frame.a = button(kBtnB);       // Switch B (south) -> Xbox A
        frame.y = button(kBtnX);       // Switch X (north) -> Xbox Y
        frame.x = button(kBtnY);       // Switch Y (west)  -> Xbox X
    } else {
        frame.a = button(kBtnA);
        frame.b = button(kBtnB);
        frame.x = button(kBtnX);
        frame.y = button(kBtnY);
    }
    frame.left_shoulder = button(kBtnL);
    frame.right_shoulder = button(kBtnR);
    frame.left_trigger = button(kBtnZL) ? 1.0f : 0.0f;
    frame.right_trigger = button(kBtnZR) ? 1.0f : 0.0f;
    frame.menu = button(kBtnPlus);
    frame.view = button(kBtnMinus);
    frame.left_thumb = button(4);
    frame.right_thumb = button(5);
    frame.dpad_left = button(kBtnLeft);
    frame.dpad_up = button(kBtnUp);
    frame.dpad_right = button(kBtnRight);
    frame.dpad_down = button(kBtnDown);
    // Both stick clicks together = Xbox nexus (guide).
    if (frame.left_thumb && frame.right_thumb) {
        frame.nexus = true;
        frame.left_thumb = frame.right_thumb = false;
    }
    auto axis = [&](int index) {
        return SDL_JoystickGetAxis(joystick, index) / 32767.0f;
    };
    frame.left_x = axis(0);
    frame.left_y = axis(1);
    frame.right_x = axis(2);
    frame.right_y = axis(3);
    if (force_nexus) frame.nexus = true;
    return frame;
}

// Drain the newest rumble command from the engine and drive the motors, then
// service the auto-stop timer. Runs every frame on the main thread. The setting
// gates it: when vibration is off we stop and never start. See SwitchRumble for
// why this goes through libnx instead of SDL_JoystickRumble.
void apply_rumble(App& app) {
#ifdef __SWITCH__
    Uint32 now = SDL_GetTicks();
    float gain = kVibrationGain[std::clamp(app.settings.vibration, 0,
                                           kVibrationLevels - 1)];
    stream::Engine::RumbleCommand cmd;
    if (app.engine->take_rumble(cmd)) {
        if (gain > 0.0f)
            app.rumble.play(cmd.low / 65535.0f, cmd.high / 65535.0f, gain,
                            cmd.duration_ms, now);
        else
            app.rumble.stop();
    }
    app.rumble.tick(now);
#else
    stream::Engine::RumbleCommand cmd;
    (void)app.engine->take_rumble(cmd);  // PC: no rumble hardware
#endif
}

// Shared error card (cards 1i/1j): top 8px error band + a boxed card with
// an "!" glyph header, message body, and optional context/log lines.
// Returns the y where extra content (suggestion box) may continue.
// Turn a raw engine/HTTP error into a short, actionable line. Unmatched errors
// fall through unchanged; the full text is always in stream-log.txt.
std::string friendly_error(const std::string& raw) {
    std::string s = lowercase(raw);
    auto has = [&](const char* n) { return s.find(n) != std::string::npos; };
    if (has("resolve host") || has("couldn't resolve") || has("could not resolve"))
        return "Couldn't reach Microsoft sign-in (DNS). Check your connection; "
               "on a shared PC or phone hotspot, set a manual DNS such as "
               "1.1.1.1 on the console.";
    if (has("timed out") || has("timeout"))
        return "The connection timed out. Check your network and try again.";
    if (has("connection refused") || has("connect to host") ||
        has("couldn't connect") || has("could not connect"))
        return "Couldn't connect to the server. Check your internet and retry.";
    if (has("agentcommanderror"))
        return "Your console didn't accept the session. Make sure it's on (or "
               "in Instant-on) and try again.";
    if (has("401") || has("403") || has("unauthorized") || has("token"))
        return "Sign-in expired or was rejected. Try signing in again.";
    return raw;
}

// Word-wrap `text` to lines no wider than max_width at `size`, drawing them from
// (x, y) downward; caps at max_lines and ellipsizes the overflow. Returns the y
// just past the last line. Fixes long errors spilling outside their card.
int draw_text_wrapped(App& app, const std::string& text, int x, int y,
                      gfx::FontSize size, gfx::Color color, int max_width,
                      int line_h, int max_lines) {
    auto width = [&](const std::string& s) { return app.gfx.text_width(s, size); };
    std::vector<std::string> words;
    for (size_t i = 0; i < text.size();) {
        while (i < text.size() && text[i] == ' ') ++i;
        size_t start = i;
        while (i < text.size() && text[i] != ' ') ++i;
        if (i > start) words.push_back(text.substr(start, i - start));
    }
    std::string line;
    int lines = 0;
    for (size_t w = 0; w < words.size(); ++w) {
        std::string cand = line.empty() ? words[w] : line + " " + words[w];
        if (width(cand) <= max_width) {
            line = cand;
            continue;
        }
        if (line.empty()) {  // a single word wider than the box: hard-truncate
            line = words[w];
            while (line.size() > 1 && width(line) > max_width) line.pop_back();
            continue;
        }
        if (lines >= max_lines - 1) {  // no more room: ellipsize and stop
            while (!line.empty() && width(line + "...") > max_width) line.pop_back();
            app.gfx.text(line + "...", x, y, size, color);
            return y + line_h;
        }
        app.gfx.text(line, x, y, size, color);
        y += line_h;
        ++lines;
        line = words[w];
        while (line.size() > 1 && width(line) > max_width) line.pop_back();
    }
    if (!line.empty()) {
        app.gfx.text(line, x, y, size, color);
        y += line_h;
    }
    return y;
}

int draw_error_card(App& app, const SDL_Rect& card, const char* title,
                    const std::string& message, const std::string& context,
                    bool show_log_path) {
    app.gfx.fill({0, 0, gfx::kWidth, 8}, gfx::kError);
    app.gfx.fill(card, gfx::kBar);
    app.gfx.frame(card, gfx::kChipEdge, 2);

    SDL_Rect icon = {card.x + 48, card.y + 36, 56, 56};
    app.gfx.fill(icon, {42, 20, 22});
    app.gfx.frame(icon, gfx::kError, 3);
    app.gfx.text_centered("!", icon.x + 28, icon.y + 10, gfx::FontSize::Body,
                          gfx::kError);
    app.gfx.text(title, icon.x + 80, card.y + 34, gfx::FontSize::Title,
                 gfx::kError);
    app.gfx.fill({card.x, card.y + 128, card.w, 2}, gfx::kChip);

    int y = card.y + 164;
    y = draw_text_wrapped(app, friendly_error(message), card.x + 48, y,
                          gfx::FontSize::Body, gfx::kText, card.w - 96, 46, 4);
    y += 12;
    if (!context.empty()) {
        app.gfx.text(context, card.x + 48, y, gfx::FontSize::Note,
                     gfx::kTextDim);
        y += 52;
    }
    if (show_log_path) {
        SDL_Rect log = {card.x + 48, y, card.w - 96, 56};
        app.gfx.fill(log, gfx::kSurface);
        app.gfx.text("log: /switch/light-is-green/stream-log.txt", log.x + 24,
                     log.y + 12, gfx::FontSize::Small, gfx::kTextDim);
        y += 76;
    }
    return y;
}

// Map the engine's status line onto the 4 real connection stages shown under
// the progress bar (card 1h).
int stream_phase(const std::string& status) {
    std::string s = lowercase(status);
    if (s.find("xbox sent no video") != std::string::npos ||
        s.find("1080p unavailable") != std::string::npos)
        return 3;
    if (s.find("handshaking") != std::string::npos ||
        s.find("secure media") != std::string::npos)
        return 2;
    if (s.find("negotiating connection") != std::string::npos ||
        s.find("no xbox route") != std::string::npos ||
        s.find("no server route") != std::string::npos ||
        s.find("webrtc route") != std::string::npos ||
        s.find("signaling failed") != std::string::npos ||
        s.find("negotiation timed out") != std::string::npos)
        return 1;
    return 0;
}

void draw_stream(App& app, SDL_Joystick* joystick) {
    stream::EngineState state = app.engine->state();

    if (state == stream::EngineState::Failed) {
        draw_header(app);
        SDL_Rect card = {460, 250, 1000, 460};
        draw_error_card(app, card, "Stream failed",
                        app.engine->error(),
                        app.launch_game.name.empty() ? app.launch_game.title_id
                                                     : app.launch_game.name,
                        true);
        draw_hints(app, {{"A", "Retry", true}, {"B", "Back to library"}});
        return;
    }

    SDL_Texture* frame = app.engine->pump_video();
    if (frame) {
        // Pure black behind video: the SDL-to-deko3d handoff never flashes a
        // coloured frame, while the connecting view can retain the v1 aurora.
        app.gfx.fill({0, 0, gfx::kWidth, gfx::kHeight}, {0, 0, 0});
        // Letterbox to preserve aspect.
        int width = app.engine->video_width();
        int height = app.engine->video_height();
        SDL_Rect destination = {0, 0, gfx::kWidth, gfx::kHeight};
        if (width > 0 && height > 0) {
            float scale = std::min(
                static_cast<float>(gfx::kWidth) / width,
                static_cast<float>(gfx::kHeight) / height);
            destination.w = static_cast<int>(width * scale);
            destination.h = static_cast<int>(height * scale);
            destination.x = (gfx::kWidth - destination.w) / 2;
            destination.y = (gfx::kHeight - destination.h) / 2;
        }
        app.gfx.draw_texture(frame, destination);
        app.engine->send_gamepad(read_gamepad(
            joystick, app.settings.mapping, xbox_home_active(app)));
        apply_rumble(app);

        if (SDL_GetTicks() < app.stream_hint_until) {
            app.gfx.fill({0, kFooterY, gfx::kWidth, kFooterH},
                         {0, 0, 0, 153});
            app.gfx.text_centered(
                "Hold  −  and  +  together to leave the stream",
                gfx::kWidth / 2, gfx::kHeight - 62, gfx::FontSize::Small,
                gfx::kTextDim);
        }
        draw_xbox_home_button(app);
        return;
    }

    // Connecting: the selected cover, real engine phases, and the effective
    // stream region. No synthetic queue, latency, or TLS claims are shown.
    draw_header(app);
    SDL_Rect launch_deck = {420, 142, 1080, 790};
    app.gfx.fill(launch_deck, {2, 15, 16, 196});
    app.gfx.frame(launch_deck, {43, 104, 94, 175}, 2);
    app.gfx.text_centered(app.launching_home ? "REMOTE PLAY" : "CLOUD LAUNCH",
                          gfx::kWidth / 2, 178, gfx::FontSize::Small,
                          gfx::kFocus);

    // A console has no game cover. The old generic fallback spelled YX and
    // looked like a broken title card, so identify Remote Play with the Light
    // is Green mark. Cloud launches retain the selected game's full artwork.
    SDL_Rect mini = app.launching_home
                        ? SDL_Rect{gfx::kWidth / 2 - 92, 300, 184, 184}
                        : SDL_Rect{gfx::kWidth / 2 - 80, 300, 160, 240};
    if (app.launching_home) {
        app.gfx.fill(mini, {5, 34, 33, 238});
        app.gfx.frame(mini, gfx::kChipEdge, 2);
        constexpr int kBrandInset = 22;
        app.gfx.draw_brand_icon({mini.x + kBrandInset, mini.y + kBrandInset,
                                 mini.w - 2 * kBrandInset,
                                 mini.h - 2 * kBrandInset});
    } else {
        SDL_Texture* cover = app.covers->get(app.launch_game.title_id,
                                              app.launch_game.box_art_url);
        if (cover)
            app.gfx.draw_texture_contain(cover, mini);
        else
            draw_cover_fallback(app, app.launch_game, mini,
                                gfx::FontSize::Title);
    }

    const std::string label = app.launching_home
                                  ? "Remote Play"
                                  : (app.launch_game.name.empty()
                                         ? app.launch_game.title_id
                                         : app.launch_game.name);
    draw_focus_frames(app, mini);
    const int title_y = app.launching_home ? 548 : 584;
    const int status_y = app.launching_home ? 614 : 650;
    const int route_y = app.launching_home ? 664 : 700;
    const int track_y = app.launching_home ? 720 : 756;
    const int stage_y = app.launching_home ? 752 : 788;
    draw_library_title_marquee(
        app, label,
        {launch_deck.x + 64, title_y, launch_deck.w - 128, 62},
                                gfx::FontSize::Title);
    draw_centered_clipped_text(
        app, app.engine->status(),
        {launch_deck.x + 64, status_y - 4, launch_deck.w - 128, 50},
        status_y, gfx::FontSize::Note, gfx::kTextDim);

    std::string route = app.launching_home
                            ? "Route: Xbox Remote Play · xHome"
                            : "Server region: " +
                                  active_stream_region_label(app);
    draw_centered_clipped_text(
        app, route,
        {launch_deck.x + 64, route_y - 4, launch_deck.w - 128, 42},
        route_y, gfx::FontSize::Small, gfx::kFocus);

    int phase = stream_phase(app.engine->status());
    SDL_Rect track = {gfx::kWidth / 2 - 280, track_y, 560, 6};
    app.gfx.fill(track, gfx::kChip);
    app.gfx.fill({track.x, track.y, 140 * (phase + 1), 6}, gfx::kFocus);

    const char* stages[4] = {"Session", "ICE", "DTLS", "Video"};
    int total = 0;
    for (int i = 0; i < 4; ++i)
        total += app.gfx.text_width(stages[i], gfx::FontSize::Small) + 32;
    int sx = (gfx::kWidth - (total - 32)) / 2;
    for (int i = 0; i < 4; ++i) {
        gfx::Color color = i < phase ? gfx::kFocus
                           : i == phase ? gfx::kText
                                        : gfx::kFaint;
        sx += app.gfx.text(stages[i], sx, stage_y, gfx::FontSize::Small, color) +
              32;
    }

    // Teach the persistent deko3d overlays before the first video frame. The
    // two bounded lines replace the old 1500px sentence that escaped both the
    // right and bottom edges of this deck in Cloud and Remote Play.
    SDL_Rect stream_help = {launch_deck.x + 48, 838,
                            launch_deck.w - 96, 78};
    app.gfx.fill(stream_help, {4, 27, 29, 222});
    app.gfx.frame(stream_help, {43, 104, 94, 145}, 1);
    draw_centered_clipped_text(
        app, "Picture controls: tap the small menu button in the top-right.",
        stream_help, stream_help.y + 8, gfx::FontSize::Small,
        gfx::kTextDim);
    draw_centered_clipped_text(
        app, "Xbox guide: tap the Xbox icon or press L3 + R3.", stream_help,
        stream_help.y + 41, gfx::FontSize::Small, gfx::kTextDim);

    // Source/quality chip, aligned under the command header.
    std::string quality =
        app.launching_home
            ? (app.launch_game.name.empty() ? std::string("Remote Play")
                                            : app.launch_game.name)
            : std::string("xCloud · ") +
                  kQualityLabels[app.settings.quality];
    int qw = std::min(460,
                      app.gfx.text_width(quality, gfx::FontSize::Small) + 40);
    SDL_Rect quality_chip = {gfx::kWidth - kMargin - qw, 154, qw, 44};
    app.gfx.fill(quality_chip,
                 {gfx::kSurface.r, gfx::kSurface.g, gfx::kSurface.b, 217});
    app.gfx.frame(quality_chip, {43, 104, 94, 145}, 1);
    draw_library_title_marquee(
        app, quality,
        {quality_chip.x + 20, quality_chip.y + 6, quality_chip.w - 40, 30},
        gfx::FontSize::Small, gfx::kTextDim);

    draw_hints(app, {{"B", "Cancel"}}, /*with_bar=*/false);
}
#endif

void draw_fatal(App& app) {
    SDL_Rect card = {410, 180, 1100,
                     app.fatal.find("streaming login") != std::string::npos
                         ? 560
                         : 400};
    int y = draw_error_card(app, card, "Something went wrong", app.fatal, "",
                            false);
    // The streaming-login step is the geo gate; if it failed, point the user
    // at Region bypass instead of leaving a dead end (card 1j).
    if (app.fatal.find("streaming login") != std::string::npos) {
        SDL_Rect tip = {card.x + 48, y, card.w - 96, 150};
        app.gfx.fill(tip, gfx::kSurface);
        app.gfx.frame(tip, gfx::kWarn, 2);
        app.gfx.fill({tip.x + 28, tip.y + 28, 8, 94}, gfx::kWarn);
        app.gfx.text("Xbox Cloud Gaming may be unavailable in your region.",
                     tip.x + 64, tip.y + 22, gfx::FontSize::Note, gfx::kText);
        app.gfx.text("Press ZL for Settings, turn on Region bypass, then X.",
                     tip.x + 64, tip.y + 74, gfx::FontSize::Note,
                     gfx::kTextDim);
    }
    draw_hints(app, {{"X", "Retry", true},
                     {"ZL", "Settings"},
                     {"−", "Sign out"},
                     {"+", "Exit"}});
}

// ---- input ----------------------------------------------------------------

struct Input {
    bool a = false, b = false, x = false, y = false;
    bool up = false, down = false, left = false, right = false;
    bool plus = false, minus = false, zl = false, zr = false;
    bool l = false, r = false;
    bool quit = false;
    bool touch = false;            // a finger tapped this frame
    int touch_x = 0, touch_y = 0;  // tap position in 1920x1080 design space
    int swipe_rows = 0;            // vertical swipe -> grid rows to scroll
    int swipe_start_x = -1;        // rail swipes must not move the game grid
};

Input poll_input(SDL_Joystick* joystick, bool direct_touch) {
    Input input;
    static float s_touch_down_x = 0, s_touch_down_y = 0;
    static bool s_touching = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) input.quit = true;
        if (event.type == SDL_FINGERDOWN) {  // remember where the touch began
            s_touch_down_x = event.tfinger.x;
            s_touch_down_y = event.tfinger.y;
            s_touching = true;
        }
        if (event.type == SDL_FINGERUP && s_touching) {
            s_touching = false;
            int dx = static_cast<int>((event.tfinger.x - s_touch_down_x) *
                                      gfx::kWidth);
            int dy = static_cast<int>((event.tfinger.y - s_touch_down_y) *
                                      gfx::kHeight);
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (ady > 120 && ady > adx) {  // vertical swipe -> scroll the grid
                input.swipe_rows = -dy / (kCardH + kGapY);
                input.swipe_start_x =
                    static_cast<int>(s_touch_down_x * gfx::kWidth);
                if (input.swipe_rows == 0) input.swipe_rows = dy < 0 ? 1 : -1;
            } else {  // a tap
                input.touch = true;
                input.touch_x = static_cast<int>(event.tfinger.x * gfx::kWidth);
                input.touch_y = static_cast<int>(event.tfinger.y * gfx::kHeight);
            }
        }
        if (event.type == SDL_JOYBUTTONDOWN) {
            switch (event.jbutton.button) {
                case kBtnA: input.a = true; break;
                case kBtnB: input.b = true; break;
                case kBtnX: input.x = true; break;
                case kBtnY: input.y = true; break;
                case kBtnUp: input.up = true; break;
                case kBtnDown: input.down = true; break;
                case kBtnLeft: input.left = true; break;
                case kBtnRight: input.right = true; break;
                case kBtnPlus: input.plus = true; break;
                case kBtnMinus: input.minus = true; break;
                case kBtnZL: input.zl = true; break;
                case kBtnZR: input.zr = true; break;
                case kBtnL: input.l = true; break;
                case kBtnR: input.r = true; break;
            }
        }
        if (event.type == SDL_KEYDOWN) {  // desktop testing
            switch (event.key.keysym.sym) {
                case SDLK_RETURN: input.a = true; break;
                case SDLK_ESCAPE: input.plus = true; break;
                case SDLK_UP: input.up = true; break;
                case SDLK_DOWN: input.down = true; break;
                case SDLK_LEFT: input.left = true; break;
                case SDLK_RIGHT: input.right = true; break;
                case SDLK_s: input.y = true; break;
                case SDLK_f: input.x = true; break;   // favorite
                case SDLK_r: input.zr = true; break;  // refresh
                case SDLK_LEFTBRACKET: input.l = true; break;
                case SDLK_RIGHTBRACKET: input.r = true; break;
            }
        }
    }
#ifdef __SWITCH__
    // SDL's video subsystem is intentionally suspended while deko3d owns the
    // display, so SDL_FINGER events are unavailable during native streaming.
    // Read the handheld touchscreen from libnx in that phase and emit the same
    // design-space tap/swipe representation as the SDL path.
    static bool s_hid_touching = false;
    static int s_hid_down_x = 0, s_hid_down_y = 0;
    static int s_hid_last_x = 0, s_hid_last_y = 0;
    if (direct_touch) {
        HidTouchScreenState state{};
        bool touching = hidGetTouchScreenStates(&state, 1) > 0 && state.count > 0;
        if (touching) {
            int x = state.touches[0].x * gfx::kWidth / 1280;
            int y = state.touches[0].y * gfx::kHeight / 720;
            if (!s_hid_touching) {
                s_hid_down_x = x;
                s_hid_down_y = y;
            }
            s_hid_touching = true;
            s_hid_last_x = x;
            s_hid_last_y = y;
        } else if (s_hid_touching) {
            s_hid_touching = false;
            int dx = s_hid_last_x - s_hid_down_x;
            int dy = s_hid_last_y - s_hid_down_y;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (ady > 120 && ady > adx) {
                input.swipe_rows = -dy / (kCardH + kGapY);
                input.swipe_start_x = s_hid_down_x;
                if (input.swipe_rows == 0) input.swipe_rows = dy < 0 ? 1 : -1;
            } else {
                input.touch = true;
                input.touch_x = s_hid_last_x;
                input.touch_y = s_hid_last_y;
            }
        }
    } else {
        s_hid_touching = false;
    }
#else
    (void)direct_touch;
#endif
    // Left analog stick also drives menu navigation: emit one directional
    // step each time the stick crosses into a deflected zone (matches the
    // one-per-press behaviour of the d-pad).
    if (joystick) {
        static int last_x = 0, last_y = 0;
        constexpr int kThreshold = 16000;
        int ax = SDL_JoystickGetAxis(joystick, 0);
        int ay = SDL_JoystickGetAxis(joystick, 1);
        int dx = ax > kThreshold ? 1 : (ax < -kThreshold ? -1 : 0);
        int dy = ay > kThreshold ? 1 : (ay < -kThreshold ? -1 : 0);
        if (dx > 0 && last_x <= 0) input.right = true;
        if (dx < 0 && last_x >= 0) input.left = true;
        if (dy > 0 && last_y <= 0) input.down = true;
        if (dy < 0 && last_y >= 0) input.up = true;
        last_x = dx;
        last_y = dy;
    }
    return input;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

#ifdef __SWITCH__
    // The default UDP receive buffer is ~42 KB, too small to absorb a 1080p
    // keyframe burst. Bump only udp_rx_buf_size (to 512 KB) and keep everything
    // else at default so the transfer-memory total stays within the bsd:u
    // service limit -- an oversized config makes socketInitialize() fail. If it
    // still fails, tear down cleanly and fall back to the guaranteed default,
    // otherwise no socket gets created at all (sendto-before-init).
    {
        SocketInitConfig cfg = *socketGetDefaultInitConfig();
        cfg.udp_rx_buf_size = 0x80000;  // 512 KB (default ~42 KB)
        if (R_FAILED(socketInitialize(&cfg))) {
            socketExit();
            socketInitializeDefault();
        }
    }
    plInitialize(PlServiceType_User);
    hidInitializeTouchScreen();  // direct reads while SDL video is suspended
    if (R_SUCCEEDED(romfsInit())) Http::set_ca_bundle("romfs:/cacert.pem");
#endif
    migrate_legacy_data_if_needed();
    mkdir(kDataDir, 0755);

    App app;
    if (!app.gfx.init()) return 1;
    app.ui_sound.init();  // menu navigation ticks (best-effort; ignored on fail)
    SDL_Joystick* joystick = SDL_JoystickOpen(0);
    app.covers = std::make_unique<Covers>(app.gfx, data_path("covers"));
    load_accounts();  // registry + migration; sets the active account
    app.auth = std::make_unique<XboxAuth>(user_path("tokens.json"));
    app.auth->set_abort_flag(&app.abort_http);
    app.settings = load_settings();
    app.server_regions = load_server_regions();
    app.auth->set_preferred_server_region(app.settings.server_region);
    app.favorites = load_id_list("favorites.json");
    app.history = load_id_list("history.json");
    {
        // Surface the previous run's last exit breadcrumb in Settings, so the
        // black-screen-on-exit bug can be diagnosed without pulling the SD.
        std::ifstream in(data_path("exit-log.txt"));
        std::string line;
        while (std::getline(in, line))
            if (!line.empty()) app.last_exit_step = line;
    }
    std::remove(data_path("exit-log.txt").c_str());
    apply_region(app.settings);  // before any network: gate opens on first call
#if defined(__SWITCH__) && defined(GNX_NATIVE_STREAM)
    app.rumble.init();  // HID is up (joystick opened) -> get vibration handles
#endif
    app.scene_started = SDL_GetTicks();
#ifdef GNX_NATIVE_STREAM
    app.engine =
        std::make_unique<stream::Engine>(*app.auth, app.gfx.renderer());
#endif

    bool running = true;
    while (running) {
        bool direct_touch = false;
#ifdef GNX_NATIVE_STREAM
        direct_touch = app.deko_active;
#endif
        Input input = poll_input(joystick, direct_touch);
        if (input.quit) break;

        // A tap on the footer hint bar acts as that button press.
        if (input.touch) {
            for (const HintHit& h : app.hint_hits) {
                if (input.touch_x >= h.rect.x &&
                    input.touch_x <= h.rect.x + h.rect.w &&
                    input.touch_y >= h.rect.y &&
                    input.touch_y <= h.rect.y + h.rect.h) {
                    const std::string& k = h.key;
                    if (k == "A") input.a = true;
                    else if (k == "B") input.b = true;
                    else if (k == "X") input.x = true;
                    else if (k == "Y") input.y = true;
                    else if (k == "L") input.l = true;
                    else if (k == "R") input.r = true;
                    else if (k == "ZL") input.zl = true;
                    else if (k == "ZR") input.zr = true;
                    input.touch = false;  // consumed by the hint bar
                    break;
                }
            }
        }

        // Snapshot navigation state; a menu tick plays below if it moved this
        // frame (grid cursor, tab, console cursor, settings row).
        int nav_cursor = app.cursor, nav_console = app.console_cursor,
            nav_settings = app.settings_cursor,
            nav_sidebar = app.library_sidebar_cursor;
        LibraryTab nav_tab = app.tab;
        bool nav_sidebar_focus = app.library_sidebar_focused;

        switch (app.scene) {
            case Scene::Splash:
                if (SDL_GetTicks() - app.scene_started > 1200) {
                    if (app.auth->has_saved_login()) {
                        app.scene = Scene::LoadingLibrary;
                        start_library_load(app, false);
                    } else {
                        app.scene = Scene::SignIn;
                        start_signin(app);
                    }
                }
                break;

            case Scene::SignIn:
                if (input.b || input.plus) {
                    // Reached from the account picker: go back to the account
                    // we came from, and drop the one we were signing into if
                    // it never got that far (an empty account is just clutter
                    // in the picker, and it used to be unremovable).
                    if (!g_signin_return_account.empty()) {
                        std::string back = g_signin_return_account;
                        g_signin_return_account.clear();
                        if (!app.auth->has_saved_login())
                            remove_account(g_active_account);
                        switch_account(app, back);
                        break;
                    }
                    app.signin_state = 4;  // cancel
                    app.abort_http = true;  // unblock an in-flight poll
                    join_worker(app);
                    running = false;
                    break;
                }
                if (input.zl) {  // reach Region bypass before the library loads
                    app.settings_return = Scene::SignIn;
                    app.scene = Scene::Settings;
                    break;
                }
                if (app.signin_state == 1) {
                    join_worker(app);
                    app.scene = Scene::LoadingLibrary;
                    start_library_load(app, false);
                } else if (app.signin_state == 2) {
                    join_worker(app);
                    start_signin(app);
                } else if (app.signin_state == 3) {
                    join_worker(app);
                    app.fatal = app.signin_error;
                    app.scene = Scene::Fatal;
                }
                break;

            case Scene::LoadingLibrary:
                if (app.load_state == 1) {
                    join_worker(app);
                    if (app.consoles.empty() &&
                        app.tab == LibraryTab::Consoles)
                        app.tab = LibraryTab::All;
                    // Preferred source = your Xbox: land on the Consoles tab
                    // (games stay one L-press away).
                    if (app.settings.source == 2 && !app.consoles.empty())
                        app.tab = LibraryTab::Consoles;
                    apply_filter(app);
                    app.library_sidebar_cursor = static_cast<int>(app.tab);
                    app.library_sidebar_focused = false;
                    app.scene = Scene::Library;
                    app.library_focus_started = SDL_GetTicks();
                    try {
                        app.gamertag = app.auth->fetch_profile().gamertag;
                        remember_gamertag(app.gamertag);
                    } catch (const std::exception&) {}
                } else if (app.load_state == 2) {
                    join_worker(app);
                    app.fatal = app.load_error;
                    app.scene = Scene::Fatal;
                }
                break;

            case Scene::Library: {
                int tabs = library_tab_count(app);
                int sidebar_search = tabs;
                int sidebar_settings = tabs + 1;
                bool console_tab = app.tab == LibraryTab::Consoles;

                // Touch follows the same persistent rail shown on screen. Card
                // taps leave rail focus and reuse the controller A path.
                if (input.touch) {
                    auto hit = [&input](const SDL_Rect& rect) {
                        return input.touch_x >= rect.x &&
                               input.touch_x < rect.x + rect.w &&
                               input.touch_y >= rect.y &&
                               input.touch_y < rect.y + rect.h;
                    };
                    bool sidebar_hit = false;
                    for (int t = 0; t < tabs; ++t) {
                        if (!hit(library_sidebar_tab_rect(t))) continue;
                        activate_library_tab(app, t);
                        app.library_sidebar_focused = false;
                        console_tab = app.tab == LibraryTab::Consoles;
                        sidebar_hit = true;
                        break;
                    }
                    if (!sidebar_hit &&
                        hit(library_sidebar_action_rect(sidebar_search, tabs))) {
                        input.y = true;
                        sidebar_hit = true;
                    }
                    if (!sidebar_hit &&
                        hit(library_sidebar_action_rect(sidebar_settings, tabs))) {
                        input.zl = true;
                        sidebar_hit = true;
                    }
                    if (!sidebar_hit && console_tab) {
                        int cx = input.touch_x - kConsoleGridX;
                        int cy = input.touch_y - kConsoleGridY;
                        if (cx >= 0 && cy >= 0 && cy < kConsoleCardH) {
                            int pitch = kConsoleCardW + kConsoleGapX;
                            int i = cx / pitch;
                            if (i < static_cast<int>(app.consoles.size()) &&
                                i < 3 && cx - i * pitch < kConsoleCardW) {
                                app.console_cursor = i;
                                app.library_sidebar_focused = false;
                                input.a = true;
                            }
                        }
                    } else if (!sidebar_hit) {
                        int gx = input.touch_x - kGridX;
                        int gy = input.touch_y - kGridY;
                        if (gx >= 0 && gy >= 0) {
                            int col = gx / (kCardW + kGapX);
                            int row = gy / (kCardH + kGapY);
                            if (col < kColumns && row < kRowsVisible &&
                                gx - col * (kCardW + kGapX) < kCardW &&
                                gy - row * (kCardH + kGapY) < kCardH) {
                                int page_start =
                                    (app.cursor / kPageSize) * kPageSize;
                                int index = page_start + row * kColumns + col;
                                if (index >= 0 &&
                                     index <
                                         static_cast<int>(app.visible.size())) {
                                    app.cursor = index;
                                    app.library_sidebar_focused = false;
                                    input.a = true;
                                }
                            }
                        }
                    }
                }

                if (input.l || input.r) {  // switch tab
                    int t = (static_cast<int>(app.tab) + (input.r ? 1 : -1) +
                             tabs) % tabs;
                    activate_library_tab(app, t);
                    app.library_sidebar_focused = false;
                    break;
                }

                if (input.y) {
                    if (console_tab) {
                        activate_library_tab(app, 0);
                        console_tab = false;
                    }
                    app.query = keyboard_input(app.query);
                    apply_filter(app);
                    app.library_sidebar_focused = false;
                    input.y = false;
                }

                bool sidebar_consumed = false;
                if (app.library_sidebar_focused) {
                    const int last_sidebar = sidebar_settings;
                    if (input.up)
                        app.library_sidebar_cursor =
                            std::max(0, app.library_sidebar_cursor - 1);
                    if (input.down)
                        app.library_sidebar_cursor =
                            std::min(last_sidebar,
                                     app.library_sidebar_cursor + 1);
                    if (input.b || input.right) {
                        app.library_sidebar_focused = false;
                    } else if (input.a) {
                        const int choice = app.library_sidebar_cursor;
                        input.a = false;
                        if (choice < tabs) {
                            activate_library_tab(app, choice);
                            console_tab = app.tab == LibraryTab::Consoles;
                            app.library_sidebar_focused = false;
                        } else if (choice == sidebar_search) {
                            app.query = keyboard_input(app.query);
                            apply_filter(app);
                        } else {
                            app.settings_return = Scene::Library;
                            app.scene = Scene::Settings;
                        }
                    }
                    sidebar_consumed = true;
                }

                // A sidebar action may have entered Settings. Do not process
                // the library's remaining shortcuts against the new scene.
                if (app.scene != Scene::Library) break;

                if (!sidebar_consumed && console_tab) {
                    int last = static_cast<int>(app.consoles.size()) - 1;
                    if (input.left && app.console_cursor == 0) {
                        app.library_sidebar_focused = true;
                        app.library_sidebar_cursor = static_cast<int>(app.tab);
                    } else if (input.left) {
                        app.console_cursor =
                            std::max(0, app.console_cursor - 1);
                    }
                    if (input.right)
                        app.console_cursor =
                            std::min(last, app.console_cursor + 1);
                    if (input.a && last >= 0) {
                        const HomeConsole& console = selected_console(app);
                        app.launch_game = Game{};
                        app.launch_game.title_id = console.server_id;
                        app.launch_game.name = console.name.empty()
                                                   ? "Your Xbox"
                                                   : console.name;
                        launch_stream(app, true);
                    }
                } else if (!sidebar_consumed) {
                    if (app.visible.empty() && input.left) {
                        app.library_sidebar_focused = true;
                        app.library_sidebar_cursor =
                            static_cast<int>(app.tab);
                        sidebar_consumed = true;
                    }
                    if (!app.visible.empty()) {
                        const int column = app.cursor % kColumns;
                        if (input.left && column == 0) {
                            app.library_sidebar_focused = true;
                            app.library_sidebar_cursor =
                                static_cast<int>(app.tab);
                        } else if (input.left) {
                            --app.cursor;
                        }
                        if (input.right && column < kColumns - 1 &&
                            app.cursor + 1 <
                                static_cast<int>(app.visible.size()))
                            ++app.cursor;
                        if (input.up && app.cursor >= kColumns)
                            app.cursor -= kColumns;
                        if (input.down && app.cursor + kColumns <
                                              static_cast<int>(app.visible.size()))
                            app.cursor += kColumns;
                    }
                    if (input.swipe_rows != 0 &&
                        input.swipe_start_x >= kGridX &&
                        !app.visible.empty()) {
                        app.cursor = std::clamp(
                            app.cursor + input.swipe_rows * kColumns, 0,
                            static_cast<int>(app.visible.size()) - 1);
                    }
                    if (input.x && !app.visible.empty()) {  // toggle favorite
                        toggle_favorite(
                            app, app.games[app.visible[app.cursor]].title_id);
                        apply_filter(app);  // Favorites tab updates live
                    }
                }
                if (input.zr) {  // refresh library from Xbox
                    app.scene = Scene::LoadingLibrary;
                    start_library_load(app, true);
                    break;
                }
                if (input.zl) {
                    app.library_sidebar_focused = false;
                    app.library_sidebar_cursor = sidebar_settings;
                    app.settings_return = Scene::Library;
                    app.scene = Scene::Settings;
                    break;
                }
                if (!console_tab && input.a && !app.visible.empty()) {
                    // Card 1f: A opens the detail screen with Play focused,
                    // so A-A still launches as fast as direct launch did.
                    app.detail_index = app.visible[app.cursor];
                    app.detail_cursor = 0;
                    app.scene = Scene::Detail;
                }
                if (input.plus) running = false;
                break;
            }

            case Scene::Detail: {
                int last = app.consoles.empty() ? 1 : 2;
                if (input.up)
                    app.detail_cursor = std::max(0, app.detail_cursor - 1);
                if (input.down)
                    app.detail_cursor = std::min(last, app.detail_cursor + 1);
                if (input.b) app.scene = Scene::Library;
                if (input.a && app.detail_index >= 0 &&
                    app.detail_index < static_cast<int>(app.games.size())) {
                    const Game& game = app.games[app.detail_index];
                    if (app.detail_cursor == 1) {
                        toggle_favorite(app, game.title_id);
                        apply_filter(app);
                    } else if (app.detail_cursor == 2) {
                        // Cycle the preferred source: Ask -> xCloud -> Xbox.
                        app.settings.source = (app.settings.source + 1) % 3;
                        save_settings(app.settings);
                    } else {
                        app.launch_game = game;
                        push_history(app, app.launch_game.title_id);
                        if (!app.consoles.empty() &&
                            app.settings.source == 0) {
                            app.pick_cursor = 0;
                            app.pick_pending = false;
                            app.scene = Scene::SourcePicker;
                        } else {
                            launch_stream(app, app.settings.source == 2);
                        }
                    }
                }
                break;
            }

            case Scene::SourcePicker: {
                if (input.left) app.pick_cursor = 0;
                if (input.right) app.pick_cursor = 1;
                if (input.b && !app.pick_pending) app.scene = Scene::Detail;
                if (input.a && !app.pick_pending) {
                    app.pick_pending = true;
                    app.pick_a_since = SDL_GetTicks();
                }
                if (app.pick_pending) {
                    // Tap = play once; hold >= 800 ms = fix as the default
                    // (settings.json) and skip this screen from now on.
                    bool held =
                        joystick && SDL_JoystickGetButton(joystick, kBtnA);
                    if (held &&
                        SDL_GetTicks() - app.pick_a_since >= 800) {
                        app.settings.source = app.pick_cursor == 1 ? 2 : 1;
                        save_settings(app.settings);
                        app.pick_pending = false;
                        launch_stream(app, app.pick_cursor == 1);
                    } else if (!held) {
                        app.pick_pending = false;
                        launch_stream(app, app.pick_cursor == 1);
                    }
                }
                break;
            }

            case Scene::Settings: {
                // Row order: cloud quality, console quality, mapping,
                // vibration, bypass, language, volume, pacing, brightness,
                // contrast, saturation, gamma, sharpness, server region,
                // [source when a console is linked], Debug HUD, accounts,
                // sign out.
                int source_row = app.consoles.empty() ? -1 : 16;
                int hud_row = app.consoles.empty() ? 16 : 17;
                int accounts_row = hud_row + 1;
                int signout_row = hud_row + 2;
                // The persistent rail remains touch-navigable in Settings.
                // Left/right stay reserved for changing the focused value;
                // controller users return to the rail with B or ZL.
                if (input.touch) {
                    auto hit = [&input](const SDL_Rect& rect) {
                        return input.touch_x >= rect.x &&
                               input.touch_x < rect.x + rect.w &&
                               input.touch_y >= rect.y &&
                               input.touch_y < rect.y + rect.h;
                    };
                    const int tabs = library_tab_count(app);
                    bool rail_action = false;
                    for (int t = 0; t < tabs; ++t) {
                        if (!hit(library_sidebar_tab_rect(t))) continue;
                        activate_library_tab(app, t);
                        app.library_sidebar_focused = false;
                        app.scene = Scene::Library;
                        rail_action = true;
                        break;
                    }
                    if (!rail_action &&
                        hit(library_sidebar_action_rect(tabs, tabs))) {
                        app.query = keyboard_input(app.query);
                        if (app.tab == LibraryTab::Consoles)
                            activate_library_tab(app, 0);
                        else
                            apply_filter(app);
                        app.scene = Scene::Library;
                        rail_action = true;
                    }
                    if (!rail_action &&
                        hit(library_sidebar_action_rect(tabs + 1, tabs)))
                        rail_action = true;  // already on Settings
                    if (rail_action) break;
                }
                if (input.up)
                    app.settings_cursor = std::max(0, app.settings_cursor - 1);
                if (input.down)
                    app.settings_cursor =
                        std::min(signout_row, app.settings_cursor + 1);
                // Leaving the row (or the screen, below) always disarms.
                if (input.up || input.down) app.signout_armed = false;
                if (input.a && app.settings_cursor == accounts_row) {
                    app.accounts_cursor = 0;
                    app.remove_armed = false;
                    app.scene = Scene::Accounts;
                    break;
                }
                if (input.a && app.settings_cursor == signout_row) {
                    if (!app.signout_armed) {
                        app.signout_armed = true;
                    } else {
                        app.signout_armed = false;
                        app.auth->logout();  // drops this account's tokens
                        for (const char* leaf : kAccountFiles)
                            std::remove(user_path(leaf).c_str());
                        // Signing out takes the account off the console. If
                        // others remain, hand over to the first of them;
                        // otherwise start fresh with a single empty account,
                        // which is what a single-account install expects.
                        std::string gone = g_active_account;
                        g_accounts.erase(
                            std::remove_if(
                                g_accounts.begin(), g_accounts.end(),
                                [&gone](const Account& account) {
                                    return account.id == gone;
                                }),
                            g_accounts.end());
                        if (g_accounts.empty())
                            g_accounts.push_back({next_account_id(), ""});
                        switch_account(app, g_accounts.front().id);
                        break;
                    }
                }
                int direction = (input.right ? 1 : 0) - (input.left ? 1 : 0);
                if (direction != 0 && app.settings_cursor != signout_row && app.settings_cursor != accounts_row) {
                    if (app.settings_cursor == 0)
                        cycle_server_region(app, direction);
                    else if (app.settings_cursor == 1) {
                        app.settings.region =
                            (app.settings.region + direction + 6) % 6;
                        apply_region(app.settings);  // takes effect next request
                    } else if (app.settings_cursor == 2)
                        app.settings.force_region =
                            (app.settings.force_region + direction + 2) % 2;
                    else if (app.settings_cursor == 3)
                        app.settings.max_bitrate =
                            (app.settings.max_bitrate + direction + 5) % 5;
                    else if (app.settings_cursor == 4)
                        app.settings.quality =
                            (app.settings.quality + direction +
                             kQualityLevels) %
                            kQualityLevels;
                    else if (app.settings_cursor == 5)
                        app.settings.console_quality =
                            (app.settings.console_quality + direction +
                             kConsoleQualityLevels) %
                            kConsoleQualityLevels;
                    else if (app.settings_cursor == 6)
                        app.settings.mapping =
                            (app.settings.mapping + direction + 2) % 2;
                    else if (app.settings_cursor == 7)
                        app.settings.vibration =
                            (app.settings.vibration + direction +
                             kVibrationLevels) %
                            kVibrationLevels;
                    else if (app.settings_cursor == 8)
                        app.settings.language =
                            (app.settings.language + direction + kLanguageCount) %
                            kLanguageCount;
                    else if (app.settings_cursor == 9)
                        app.settings.volume = std::clamp(
                            app.settings.volume + direction * 0.5f, 0.5f, 4.0f);
                    else if (app.settings_cursor == 10)
                        app.settings.pacing =
                            (app.settings.pacing + direction + kPacingLevels) %
                            kPacingLevels;
                    else if (app.settings_cursor == 11)
                        app.settings.brightness = std::clamp(
                            app.settings.brightness + direction * 5, -20, 20);
                    else if (app.settings_cursor == 12)
                        app.settings.contrast = std::clamp(
                            app.settings.contrast + direction * 10, 70, 130);
                    else if (app.settings_cursor == 13)
                        app.settings.saturation = std::clamp(
                            app.settings.saturation + direction * 10, 0, 150);
                    else if (app.settings_cursor == 14)
                        app.settings.gamma = std::clamp(
                            app.settings.gamma + direction * 5, 50, 200);
                    else if (app.settings_cursor == 15)
                        app.settings.sharpness =
                            (app.settings.sharpness + direction + 4) % 4;
                    else if (source_row != -1 && app.settings_cursor == source_row)
                        app.settings.source =
                            (app.settings.source + direction + 3) % 3;
                    else if (app.settings_cursor == hud_row)
                        app.settings.debug_hud = app.settings.debug_hud ? 0 : 1;
                    save_settings(app.settings);
                }
                if (input.b || input.zl) {
                    app.signout_armed = false;
                    app.scene = app.settings_return;
                }
                break;
            }

            case Scene::Accounts: {
                int add_row = static_cast<int>(g_accounts.size());
                if (input.up)
                    app.accounts_cursor = std::max(0, app.accounts_cursor - 1);
                if (input.down)
                    app.accounts_cursor =
                        std::min(add_row, app.accounts_cursor + 1);
                if (input.up || input.down) app.remove_armed = false;
                // X removes another account from this console: its sign-in and
                // cached library go, the console keeps everything else. Two
                // presses, like Sign out. The active account leaves through
                // Sign out instead, which has a token to drop as well.
                if (input.x && app.accounts_cursor < add_row &&
                    g_accounts[app.accounts_cursor].id != g_active_account) {
                    if (!app.remove_armed) {
                        app.remove_armed = true;
                    } else {
                        app.remove_armed = false;
                        remove_account(g_accounts[app.accounts_cursor].id);
                        app.accounts_cursor =
                            std::min(app.accounts_cursor,
                                     static_cast<int>(g_accounts.size()));
                    }
                    break;
                }
                if (input.a) {
                    app.remove_armed = false;
                    if (app.accounts_cursor == add_row)
                        add_account(app);  // lands on the sign-in screen
                    else if (g_accounts[app.accounts_cursor].id !=
                             g_active_account)
                        switch_account(app, g_accounts[app.accounts_cursor].id);
                    else
                        app.scene = Scene::Settings;  // already the active one
                    break;
                }
                if (input.b) {
                    app.remove_armed = false;
                    app.scene = Scene::Settings;
                }
                break;
            }

#ifdef GNX_NATIVE_STREAM
            case Scene::Stream: {
                stream::EngineState stream_state = app.engine->state();
                bool streaming =
                    stream_state == stream::EngineState::Streaming;

                if (streaming) {
                    bool quick_touch = input.touch &&
                        handle_quick_menu_touch(
                            app, input.touch_x, input.touch_y);
                    if (input.touch && !quick_touch &&
                        point_in_rect(input.touch_x, input.touch_y,
                                      kXboxHomeRect)) {
                        // Hold Nexus for several 125 Hz reports. A single tap
                        // packet can be too brief for the remote guide to open.
                        app.xbox_home_until = SDL_GetTicks() + 160;
                        app.ui_sound.play(1.0f);
                    }
                    // Exit combo: - and + held together.
                    bool minus_held =
                        joystick && SDL_JoystickGetButton(joystick, kBtnMinus);
                    bool plus_held =
                        joystick && SDL_JoystickGetButton(joystick, kBtnPlus);
                    if (minus_held && plus_held) {
                        app.engine->stop();
                        app.scene = Scene::Library;
                    }
                } else if (stream_state == stream::EngineState::Stopped) {
                    // The server ended the session (stream stopped on the
                    // console, console switched off). Nothing to retry and
                    // nothing to show: go straight back to the library.
                    app.engine->stop();
                    app.scene = Scene::Library;
                } else if (stream_state == stream::EngineState::Failed) {
                    if (input.b) {
                        app.engine->stop();
                        app.scene = Scene::Library;
                    }
                    if (input.a)  // retry the same target
                        launch_stream(app, app.launching_home);
                } else if (input.b) {  // cancel while connecting
                    app.engine->stop();
                    app.scene = Scene::Library;
                }
                break;
            }
#else
            case Scene::Stream:
                app.scene = Scene::Library;
                break;
#endif

            case Scene::Fatal:
                if (input.zl) {  // enable Region bypass, then X to retry
                    app.settings_return = Scene::Fatal;
                    app.scene = Scene::Settings;
                    break;
                }
                if (input.x) {
                    app.scene = Scene::LoadingLibrary;
                    start_library_load(app, true);
                }
                if (input.minus) {
                    app.auth->logout();
                    app.scene = Scene::SignIn;
                    start_signin(app);
                }
                if (input.plus || input.b) running = false;
                break;
        }

#ifdef GNX_NATIVE_STREAM
        // Hand the single Switch display between SDL (menus/status) and deko3d
        // (zero-copy video). We switch to deko3d once the first frame is ready
        // and switch back when the streaming phase ends.
        bool want_deko =
            app.scene == Scene::Stream &&
            app.engine->state() == stream::EngineState::Streaming;
        if (want_deko && !app.deko_active) {
            app.covers->drop_textures();  // textures die with the renderer
            app.gfx.suspend();
            if (app.engine->begin_deko_output()) {
                app.deko_active = true;
            } else {
                app.gfx.resume();  // deko3d unavailable -> stay on SDL
            }
        }
        if (!want_deko && app.deko_active) {
            app.engine->end_deko_output();  // release the swapchain first
            app.gfx.resume();
            app.deko_active = false;
#ifdef __SWITCH__
            app.rumble.stop();  // motors off when the stream ends
#endif
        }
        if (app.deko_active) {
            // pump_video decodes everything queued and presents the freshest
            // frame on its own ~60 Hz software clock (it does NOT block on the
            // GPU/vsync -- deko3d's waitIdle only waits for the GPU, and blocking
            // on acquireImage instead crashed). So this loop must run fast and
            // yield 1 ms per spin, or it busy-waits at 100% CPU. Presentation
            // pacing lives entirely in pump_video, decoupled from this loop rate.
            const bool guide_pressed = xbox_home_active(app);
            app.engine->set_guide_button_pressed(guide_pressed);
            app.engine->pump_video();
            // Pace input at ~125 Hz. The loop spins far faster than the video
            // rate; sending a gamepad packet every spin floods the SCTP input
            // channel ("sctp sendv error 11").
            Uint32 now = SDL_GetTicks();
            if (now - app.last_input_ms >= 8) {
                app.engine->send_gamepad(read_gamepad(
                    joystick, app.settings.mapping, guide_pressed));
                apply_rumble(app);
                app.last_input_ms = now;
            }
            SDL_Delay(1);  // yield between spins; present cadence is timer-driven
            continue;      // deko3d owns the frame; no SDL pass this iteration
        }
#endif

        bool navigation_changed =
            app.cursor != nav_cursor || app.tab != nav_tab ||
            app.console_cursor != nav_console ||
            app.settings_cursor != nav_settings ||
            app.library_sidebar_cursor != nav_sidebar ||
            app.library_sidebar_focused != nav_sidebar_focus;
        if (navigation_changed)
            app.ui_sound.play(1.0f);
        if (navigation_changed && app.scene == Scene::Library)
            app.library_focus_started = SDL_GetTicks();

        app.covers->pump();
        app.gfx.begin_frame();
        switch (app.scene) {
            case Scene::Splash: draw_splash(app); break;
            case Scene::SignIn: draw_signin(app); break;
            case Scene::LoadingLibrary: draw_loading(app); break;
            case Scene::Library: draw_library(app); break;
            case Scene::Detail: draw_detail(app); break;
            case Scene::SourcePicker: draw_source_picker(app); break;
            case Scene::Settings: draw_settings(app); break;
            case Scene::Accounts: draw_accounts(app); break;
            case Scene::Stream:
#ifdef GNX_NATIVE_STREAM
                draw_stream(app, joystick);
#endif
                break;
            case Scene::Fatal: draw_fatal(app); break;
        }
        app.gfx.end_frame();
    }

    // Exit breadcrumbs: if the screen goes black on exit, this file shows the
    // last step reached (hang) or "done" (clean exit -> black is the title-
    // takeover launch artifact, press HOME).
    auto breadcrumb = [](const char* step) {
#ifdef __SWITCH__
        FILE* f = std::fopen("sdmc:/switch/light-is-green/exit-log.txt", "a");
        if (f) { std::fprintf(f, "%s\n", step); std::fclose(f); }
#else
        (void)step;
#endif
    };
    breadcrumb("--- exit begin");

    if (app.signin_state == 0) app.signin_state = 4;
    app.abort_http = true;  // unblock any in-flight worker HTTP call
    join_worker(app);
    breadcrumb("workers joined");

#ifdef GNX_NATIVE_STREAM
    app.engine.reset();
    breadcrumb("engine stopped");
    // Stops usrsctp's service threads. They are not ours to join, and if they
    // are still alive when hbloader unmaps the NRO the process crashes on the
    // way back to the menu.
    stream::Engine::global_shutdown();
    breadcrumb("webrtc shut down");
#endif
    app.covers.reset();
    breadcrumb("covers stopped");
    // Every libcurl handle has to be gone before socketExit(). A handle keeps
    // its TLS connections open in its own cache, and socketExit() closes bsd:u
    // and unmaps the socket transfer memory those live sockets are still using
    // -- that is the black screen on the way out. The engine and the cover
    // downloader own handles too, hence the order here; auth's handle survived
    // until App's destructor, which runs after the services are already gone.
    app.auth.reset();
    Http::global_cleanup();
    breadcrumb("network released");
    app.gfx.shutdown();
    breadcrumb("gfx shut down");
#ifdef __SWITCH__
    // One breadcrumb per service: a black screen on exit hangs somewhere in
    // here, and "gfx shut down" alone doesn't say which call never returned.
    romfsExit();
    breadcrumb("romfs exit");
    plExit();
    breadcrumb("pl exit");
    socketExit();
    breadcrumb("socket exit");
#endif
    breadcrumb("done");
    return 0;
}
