// SPDX-License-Identifier: MIT
//
// vizsetup: the intro screen of viz1090 on the cyberdeck (any screen from 640x480 up, touch and keyboard).
//
// Shows whether the RTL-SDR dongle and a GPS receiver are connected, takes the position (the code of the
// nearest airport typed on the keyboard, the GPS, or latitude / longitude on a touch keypad), saves it to
// ~/.config/cardputerzero/viz1090.conf and exits with 0 (start viz1090) or 1 (cancelled).
// The layout is the 640x480 one, stretched to the screen: positions and heights follow the screen height, the
// left column takes the extra width, text and keys grow with the smaller of the two ratios. Below 640x480 it only
// shows a "needs 640x480" notice.
//
//   vizsetup            the intro screen (the window is the screen, see viz_screen.h)
//   vizsetup --screen   prints "W H UISCALE PPMM_X100 SOURCE" for run_viz1090.sh, and exits
//
// Runs through the same display / input bridge as viz1090 (libviz_fb.so, SDL_VIDEODRIVER=offscreen).
// Environment for tests: VIZ_PROGRAM (the viz1090 binary that must exist, default /opt/viz1090/viz1090),
// VIZ_WORLD, VIZ_FONT, VIZ_LAYOUT_REPORT=1 (prints every touch target and its size in mm once).
// Build: g++ -O2 -std=c++11 vizsetup.cpp -o vizsetup -lSDL2 -lSDL2_ttf -lSDL2_gfx -lpthread

#include <SDL2/SDL.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "viz_screen.h"

namespace {

const char *kWorldMap = "/opt/viz1090/world-map.bin";
const char *kFont = "/opt/viz1090/font/Montserrat-Medium.ttf";

const SDL_Color kBg{0x0B, 0x0F, 0x12, 255};
const SDL_Color kPanel{0x18, 0x1D, 0x21, 255};
const SDL_Color kGold{0xF0, 0xB4, 0x00, 255};
const SDL_Color kGreen{0x33, 0xCC, 0x33, 255};
const SDL_Color kRed{0xE0, 0x52, 0x52, 255};
const SDL_Color kAmber{0xF0, 0xA0, 0x30, 255};
const SDL_Color kMuted{0x8A, 0x92, 0x9B, 255};
const SDL_Color kBlue{0x3B, 0x9D, 0xFF, 255};
const SDL_Color kKey{0x2C, 0x33, 0x38, 255};
const SDL_Color kKey2{0x3A, 0x41, 0x48, 255};
const SDL_Color kWhite{255, 255, 255, 255};
const SDL_Color kBlack{0, 0, 0, 255};
const SDL_Color kDim{0x55, 0x55, 0x55, 255};

std::string read_line(const std::string &path)
{
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

// ---------------------------------------------------------------------------------- SDR dongle

struct Id { const char *vid; const char *pid; };
const Id kSdrIds[] = {
    {"0bda", "2832"}, {"0bda", "2838"}, {"0413", "6680"}, {"0413", "6f0f"}, {"0458", "707f"}, {"0ccd", "00a9"},
    {"0ccd", "00b3"}, {"0ccd", "00b4"}, {"0ccd", "00b5"}, {"0ccd", "00b7"}, {"0ccd", "00b8"}, {"0ccd", "00b9"},
    {"0ccd", "00c0"}, {"0ccd", "00c6"}, {"0ccd", "00d3"}, {"0ccd", "00d7"}, {"0ccd", "00e0"}, {"1554", "5020"},
    {"15f4", "0131"}, {"15f4", "0133"}, {"185b", "0620"}, {"185b", "0650"}, {"185b", "0680"}, {"1b80", "d393"},
    {"1b80", "d394"}, {"1b80", "d395"}, {"1b80", "d397"}, {"1b80", "d398"}, {"1b80", "d39d"}, {"1b80", "d3a4"},
    {"1b80", "d3a8"}, {"1b80", "d3af"}, {"1b80", "d3b0"}, {"1d19", "1101"}, {"1d19", "1102"}, {"1d19", "1103"},
    {"1d19", "1104"}, {"1f4d", "a803"}, {"1f4d", "b803"}, {"1f4d", "c803"}, {"1f4d", "d286"}, {"1f4d", "d803"},
};

// The dongle's name, or "" when none is plugged in.
std::string find_sdr()
{
    DIR *dir = opendir("/sys/bus/usb/devices");
    if (!dir) return "";
    std::string found;
    while (dirent *entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::string base = std::string("/sys/bus/usb/devices/") + entry->d_name;
        const std::string vid = read_line(base + "/idVendor");
        const std::string pid = read_line(base + "/idProduct");
        if (vid.empty()) continue;
        for (const Id &id : kSdrIds) {
            if (vid == id.vid && pid == id.pid) {
                const std::string product = read_line(base + "/product");
                const std::string maker = read_line(base + "/manufacturer");
                found = !maker.empty() && maker != "Realtek" ? maker + " " + product : product;
                if (found.empty()) found = vid + ":" + pid;
                break;
            }
        }
        if (!found.empty()) break;
    }
    closedir(dir);
    return found;
}

// ------------------------------------------------------------------------------------- GPS

double nmea_degrees(const std::string &value, const std::string &hemisphere)
{
    if (value.size() < 4) return 0;
    const double raw = std::atof(value.c_str());
    const double degrees = std::floor(raw / 100.0);
    double result = degrees + (raw - degrees * 100.0) / 60.0;
    if (hemisphere == "S" || hemisphere == "W") result = -result;
    return result;
}

std::vector<std::string> split(const std::string &line, char separator)
{
    std::vector<std::string> out;
    std::string current;
    for (char c : line) {
        if (c == separator) { out.push_back(current); current.clear(); }
        else if (c != '\r' && c != '\n') current.push_back(c);
    }
    out.push_back(current);
    return out;
}

speed_t baud_constant(int baud)
{
    switch (baud) {
    case 4800: return B4800;
    case 38400: return B38400;
    case 115200: return B115200;
    default: return B9600;
    }
}

struct GpsState {
    std::mutex mutex;
    std::atomic<bool> stop{false};
    int state = 0;                 // 0 none, 1 NMEA seen but no fix yet, 2 fix
    std::string port, name;
    double lat = 0, lon = 0;
    int sats = 0;
};

struct NmeaResult {
    bool seen = false, fix = false;
    double lat = 0, lon = 0;
    int sats = 0;
};

// Listens to a serial port for a moment and looks for NMEA sentences (it never writes to the port).
NmeaResult read_nmea(const std::string &path, int baud, int milliseconds, const std::atomic<bool> &stop)
{
    NmeaResult result;
    const int fd = open(path.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return result;
    termios tio{};
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, baud_constant(baud));
        cfsetospeed(&tio, baud_constant(baud));
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio);
    }
    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!stop && std::chrono::steady_clock::now() < deadline) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 200) > 0 && (p.revents & POLLIN)) {
            char chunk[256];
            const ssize_t got = read(fd, chunk, sizeof(chunk));
            if (got <= 0) break;
            buffer.append(chunk, static_cast<size_t>(got));
        }
        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            const std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (line.size() < 8 || line[0] != '$') continue;
            const auto f = split(line.substr(0, line.find('*')), ',');
            if (f[0].size() < 6) continue;
            const std::string kind = f[0].substr(3);
            if (kind != "GGA" && kind != "RMC" && kind != "GLL" && kind != "GSV" && kind != "GSA" && kind != "VTG") continue;
            result.seen = true;
            if (kind == "GGA" && f.size() > 7 && std::atoi(f[6].c_str()) > 0 && !f[2].empty()) {
                result.fix = true;
                result.lat = nmea_degrees(f[2], f[3]);
                result.lon = nmea_degrees(f[4], f[5]);
                result.sats = std::atoi(f[7].c_str());
            } else if (kind == "RMC" && f.size() > 6 && f[2] == "A" && !f[3].empty()) {
                result.fix = true;
                result.lat = nmea_degrees(f[3], f[4]);
                result.lon = nmea_degrees(f[5], f[6]);
            }
        }
        if (result.fix) break;
    }
    close(fd);
    return result;
}

std::vector<std::string> serial_candidates()
{
    std::vector<std::string> ports;
    DIR *dir = opendir("/dev");
    if (!dir) return ports;
    while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("ttyACM", 0) == 0 || name.rfind("ttyUSB", 0) == 0) ports.push_back("/dev/" + name);
    }
    closedir(dir);
    std::sort(ports.begin(), ports.end());
    return ports;
}

// Background thread: finds a USB GPS receiver (a serial port that talks NMEA) and its position.
void gps_worker(std::shared_ptr<GpsState> state)
{
    int preferred_baud = 9600;
    while (!state->stop) {
        NmeaResult best;
        std::string best_port;
        for (const std::string &port : serial_candidates()) {
            if (state->stop) break;
            const int bauds[] = {preferred_baud, 9600, 4800, 38400, 115200};
            for (size_t i = 0; i < 5 && !state->stop; ++i) {
                if (i > 0 && bauds[i] == preferred_baud) continue;
                const NmeaResult r = read_nmea(port, bauds[i], i == 0 ? 2500 : 1500, state->stop);
                if (r.seen) {
                    preferred_baud = bauds[i];
                    if (!best.seen || r.fix) { best = r; best_port = port; }
                    break;
                }
            }
            if (best.fix) break;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!best.seen) {
                state->state = 0;
                state->port.clear();
            } else {
                state->state = best.fix ? 2 : 1;
                state->port = best_port;
                const std::string tty = best_port.substr(best_port.rfind('/') + 1);
                state->name = read_line("/sys/class/tty/" + tty + "/device/../product");
                if (best.fix) { state->lat = best.lat; state->lon = best.lon; state->sats = best.sats; }
            }
        }
        for (int i = 0; i < 40 && !state->stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ------------------------------------------------------------------------- saved position

std::string config_path()
{
    const char *base = std::getenv("XDG_CONFIG_HOME");
    const std::string dir = base && base[0] ? std::string(base)
                                            : std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") + "/.config";
    return dir + "/cardputerzero/viz1090.conf";
}

void load_position(std::string &lat, std::string &lon, std::string &airport)
{
    std::ifstream file(config_path());
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("LAT=", 0) == 0) lat = line.substr(4);
        else if (line.rfind("LON=", 0) == 0) lon = line.substr(4);
        else if (line.rfind("AIRPORT=", 0) == 0) airport = line.substr(8);
    }
}

bool save_position(const std::string &lat, const std::string &lon, const std::string &airport)
{
    const std::string path = config_path();
    std::vector<std::string> kept;
    {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line))
            if (line.rfind("LAT=", 0) != 0 && line.rfind("LON=", 0) != 0 && line.rfind("AIRPORT=", 0) != 0 && !line.empty())
                kept.push_back(line);
    }
    const std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file << "LAT=" << lat << std::endl;
    file << "LON=" << lon << std::endl;
    if (!airport.empty()) file << "AIRPORT=" << airport << std::endl;
    for (const std::string &line : kept) file << line << std::endl;
    return static_cast<bool>(file);
}

bool parse_degrees(const std::string &text, double limit, double &out)
{
    if (text.empty() || text == "-" || text == ".") return false;
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (!end || *end != '\0' || std::fabs(value) > limit) return false;
    out = value;
    return true;
}

std::string degrees_text(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.5f", value);
    return buffer;
}

struct Airport {
    std::string code;
    double lat = 0, lon = 0;
};

// IATA code and position of every airport, from the world map file (see make_world.py).
std::vector<Airport> load_airports()
{
    std::vector<Airport> airports;
    const char *env = std::getenv("VIZ_WORLD");
    std::ifstream file(env && env[0] ? env : kWorldMap, std::ios::binary);
    char header[28];
    if (!file || !file.read(header, sizeof(header)) || std::memcmp(header, "VIZWORLD", 8) != 0) return airports;
    uint32_t counts[5];
    std::memcpy(counts, header + 8, sizeof(counts));      // chunks, points, airports, runways, places
    file.seekg(static_cast<std::streamoff>(28ull + static_cast<uint64_t>(counts[0]) * 24 + static_cast<uint64_t>(counts[1]) * 8));
    for (uint32_t i = 0; i < counts[2]; ++i) {
        char record[16];
        if (!file.read(record, sizeof(record))) break;
        int32_t lat, lon;
        std::memcpy(&lat, record, 4);
        std::memcpy(&lon, record + 4, 4);
        Airport airport;
        airport.code.assign(record + 8, strnlen(record + 8, 8));
        airport.lat = lat / 10000.0;
        airport.lon = lon / 10000.0;
        if (!airport.code.empty()) airports.push_back(airport);
    }
    return airports;
}

// -------------------------------------------------------------------------------- drawing

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

enum Id2 {
    K0, K1, K2, K3, K4, K5, K6, K7, K8, K9, KSign, KDot, KBack, KNext,
    FieldAirport, FieldLat, FieldLon, GpsPanel, BtnBack, BtnStart, None
};
enum class Field { Airport, Lat, Lon };

const char *id_name(int id)
{
    static const char *names[] = {"key 0", "key 1", "key 2", "key 3", "key 4", "key 5", "key 6", "key 7", "key 8",
                                  "key 9", "key +/-", "key .", "key backspace", "key Next", "airport field",
                                  "latitude field", "longitude field", "GPS panel", "Back", "Start", "none"};
    return id >= 0 && id <= None ? names[id] : "?";
}

struct Hit { Rect rect; int id; };

class Setup {
public:
    int run();

private:
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    std::map<int, TTF_Font *> fonts_;
    std::map<std::string, SDL_Texture *> texture_cache_;
    std::vector<Hit> hits_;
    int pressed_ = None;

    // the screen, and the scales of the 640x480 design: horizontal, vertical, sizes (text, radii, keys)
    viz_screen_t screen_{};
    int w_ = 640, h_ = 480;
    float sx_ = 1, sy_ = 1, s_ = 1;
    bool too_small_ = false;
    bool reported_ = false;

    std::string lat_, lon_, airport_, picked_;
    Field field_ = Field::Airport;
    std::vector<Airport> airports_;
    std::shared_ptr<GpsState> gps_ = std::make_shared<GpsState>();
    bool installed_ = false;
    bool gps_fix_ = false;
    double gps_lat_ = 0, gps_lon_ = 0;
    std::string message_;
    bool done_ = false;
    int result_ = 1;

    int S(float v) const { return static_cast<int>(std::lround(v * s_)); }   // a size of the design
    int Y(float v) const { return static_cast<int>(std::lround(v * sy_)); }  // a row of the design
    // offset of a line inside a box that was `design_h` high and is `actual_h` now: scaled, and centred in the extra
    int inside(float design_off, float design_h, int actual_h) const
    {
        return static_cast<int>(std::lround(design_off * s_ + (actual_h - design_h * s_) / 2));
    }

    TTF_Font *font(int px);
    void text(const std::string &value, int px, SDL_Color color, int x, int y, int max_width = 0, bool right = false);
    int text_width(const std::string &value, int px);
    void panel(Rect r, SDL_Color color, int radius);
    void outline(Rect r, SDL_Color color);
    void button(Rect r, const std::string &label, int px, SDL_Color color, SDL_Color text_color, int id, Rect hit);
    void draw();
    void draw_notice();
    void report_layout();
    void activate(int id);
    void type_char(char ch);
    void type_airport(char ch);
    void backspace();
    void next_field(int step);
    void use_gps();
    void start();
    bool position(double &lat, double &lon) const;
    void handle(const SDL_Event &event);
    void clear_cache();
};

TTF_Font *Setup::font(int px)
{
    auto it = fonts_.find(px);
    if (it != fonts_.end()) return it->second;
    const char *env = std::getenv("VIZ_FONT");
    TTF_Font *f = TTF_OpenFont(env && env[0] ? env : kFont, px);
    fonts_[px] = f;
    return f;
}

int Setup::text_width(const std::string &value, int px)
{
    int w = 0, h = 0;
    if (TTF_Font *f = font(px)) TTF_SizeUTF8(f, value.c_str(), &w, &h);
    return w;
}

void Setup::clear_cache()
{
    for (auto &entry : texture_cache_) SDL_DestroyTexture(entry.second);
    texture_cache_.clear();
}

void Setup::text(const std::string &value, int px, SDL_Color color, int x, int y, int max_width, bool right)
{
    if (value.empty()) return;
    std::string shown = value;
    if (max_width > 0)
        while (shown.size() > 1 && text_width(shown, px) > max_width) {
            shown.pop_back();
            if (text_width(shown + "...", px) <= max_width) { shown += "..."; break; }
        }
    char key[96];
    std::snprintf(key, sizeof(key), "%d|%02x%02x%02x|", px, color.r, color.g, color.b);
    const std::string cache_key = std::string(key) + shown;
    SDL_Texture *texture = nullptr;
    auto it = texture_cache_.find(cache_key);
    if (it != texture_cache_.end()) {
        texture = it->second;
    } else {
        TTF_Font *f = font(px);
        if (!f) return;
        SDL_Surface *surface = TTF_RenderUTF8_Blended(f, shown.c_str(), color);
        if (!surface) return;
        texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_FreeSurface(surface);
        if (!texture) return;
        if (texture_cache_.size() > 300) clear_cache();
        texture_cache_[cache_key] = texture;
    }
    int w = 0, h = 0;
    SDL_QueryTexture(texture, nullptr, nullptr, &w, &h);
    SDL_Rect dst{right ? x - w : x, y, w, h};
    SDL_RenderCopy(renderer_, texture, nullptr, &dst);
}

void Setup::panel(Rect r, SDL_Color color, int radius)
{
    roundedBoxRGBA(renderer_, r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, radius, color.r, color.g, color.b, 255);
}

// The focus frame of a field: 3 px on the deck.
void Setup::outline(Rect r, SDL_Color color)
{
    const int thickness = std::max(2, S(3));
    for (int i = 0; i < thickness; ++i)
        rectangleRGBA(renderer_, r.x + i, r.y + i, r.x + r.w - 1 - i, r.y + r.h - 1 - i, color.r, color.g, color.b, 255);
}

// A button; `hit` is the area a tap selects it in (the button and its share of the gaps around it).
void Setup::button(Rect r, const std::string &label, int px, SDL_Color color, SDL_Color text_color, int id, Rect hit)
{
    if (pressed_ == id) color = SDL_Color{0x70, 0x70, 0x70, 255};
    panel(r, color, S(14));
    const int w = text_width(label, px);
    const int h = font(px) ? TTF_FontHeight(font(px)) : px;
    text(label, px, text_color, r.x + (r.w - w) / 2, r.y + (r.h - h) / 2);
    hits_.push_back({hit, id});
}

bool Setup::position(double &lat, double &lon) const
{
    return parse_degrees(lat_, 90.0, lat) && parse_degrees(lon_, 180.0, lon);
}

// Every number below is the 640x480 design: S() scales a size, Y() a row; the left column takes the extra width.
void Setup::draw()
{
    SDL_SetRenderDrawColor(renderer_, kBg.r, kBg.g, kBg.b, 255);
    SDL_RenderClear(renderer_);
    hits_.clear();

    // keypad on the right (3 columns of 88 + 8 on the deck), the left column in the rest
    const int key_pitch = S(96), key_w = S(88), key_gap = key_pitch - key_w;
    const int keypad_w = 2 * key_pitch + key_w;
    const int keypad_x = w_ - S(20) - keypad_w;
    const int left = S(16);
    const int column = keypad_x - S(20) - left;      // 304 on the deck

    text("viz1090", S(28), kGold, left, S(6));
    // SDR
    static std::string sdr;
    static Uint32 last_scan = 0;
    if (SDL_GetTicks() - last_scan > 1000 || last_scan == 0) { sdr = find_sdr(); last_scan = SDL_GetTicks(); }
    const Rect sdr_box{left, Y(52), column, Y(56)};
    panel(sdr_box, kPanel, S(12));
    const SDL_Color sdr_color = sdr.empty() ? kRed : kGreen;
    filledCircleRGBA(renderer_, left + S(16), sdr_box.y + sdr_box.h / 2, S(9), sdr_color.r, sdr_color.g, sdr_color.b, 255);
    text("SDR receiver", S(14), kMuted, left + S(36), sdr_box.y + inside(3, 56, sdr_box.h));
    text(sdr.empty() ? "Not found: plug it in" : sdr, S(20), sdr.empty() ? kRed : kWhite, left + S(36),
         sdr_box.y + inside(22, 56, sdr_box.h), column - S(48));

    // GPS (tap the panel to use its position)
    int state = 0, sats = 0;
    std::string port, name;
    {
        std::lock_guard<std::mutex> lock(gps_->mutex);
        state = gps_->state;
        port = gps_->port;
        name = gps_->name;
        sats = gps_->sats;
        gps_fix_ = state == 2;
        if (gps_fix_) { gps_lat_ = gps_->lat; gps_lon_ = gps_->lon; }
    }
    const Rect gps_box{left, Y(114), column, Y(56)};
    panel(gps_box, kPanel, S(12));
    SDL_Color gps_color = state == 2 ? kGreen : state == 1 ? kAmber : kMuted;
    filledCircleRGBA(renderer_, left + S(16), gps_box.y + gps_box.h / 2, S(9), gps_color.r, gps_color.g, gps_color.b, 255);
    text("GPS receiver (tap to use)", S(14), kMuted, left + S(36), gps_box.y + inside(3, 56, gps_box.h));
    char line[96];
    if (state == 0) std::snprintf(line, sizeof(line), "None detected");
    else if (state == 1) std::snprintf(line, sizeof(line), "%s: waiting for a fix", name.empty() ? port.c_str() : name.c_str());
    else std::snprintf(line, sizeof(line), "Fix %.4f, %.4f (%d sats)", gps_lat_, gps_lon_, sats);
    text(line, S(20), state == 0 ? kMuted : state == 1 ? kAmber : kWhite, left + S(36),
         gps_box.y + inside(22, 56, gps_box.h), column - S(48));
    hits_.push_back({{left, Y(111), column, Y(176) - Y(111)}, GpsPanel});   // down to the label below

    // airport code
    const Rect airport_box{left, Y(200), S(150), Y(56)};
    text("Nearest airport code", S(14), kMuted, left, airport_box.y - S(21));
    panel(airport_box, kPanel, S(12));
    if (field_ == Field::Airport) outline(airport_box, kBlue);
    text(airport_.empty() ? "CDG" : airport_, S(32), airport_.empty() ? kDim : kWhite, left + S(14),
         airport_box.y + inside(8, 56, airport_box.h));
    hits_.push_back({{left, Y(176), column, Y(260) - Y(176)}, FieldAirport});   // label, field and the line beside it
    const int beside = airport_box.x + airport_box.w + S(12), beside_y = airport_box.y + inside(16, 56, airport_box.h);
    if (airports_.empty()) text("no airport list", S(20), kMuted, beside, beside_y);
    else if (!picked_.empty() && picked_ == airport_) text("found", S(20), kGreen, beside, beside_y);
    else if (airport_.size() >= 3) text("unknown code", S(20), kRed, beside, beside_y);

    // latitude / longitude
    const int half = (column - S(8)) / 2;
    const Rect lat_box{left, Y(288), half, Y(50)}, lon_box{left + half + S(8), Y(288), half, Y(50)};
    text("Latitude", S(14), kMuted, lat_box.x, lat_box.y - S(21));
    text("Longitude", S(14), kMuted, lon_box.x, lon_box.y - S(21));
    panel(lat_box, kPanel, S(12));
    panel(lon_box, kPanel, S(12));
    if (field_ == Field::Lat) outline(lat_box, kBlue);
    if (field_ == Field::Lon) outline(lon_box, kBlue);
    text(lat_.empty() ? "--" : lat_, S(20), lat_.empty() ? kDim : kWhite, lat_box.x + S(12),
         lat_box.y + inside(12, 50, lat_box.h), lat_box.w - S(24));
    text(lon_.empty() ? "--" : lon_, S(20), lon_.empty() ? kDim : kWhite, lon_box.x + S(12),
         lon_box.y + inside(12, 50, lon_box.h), lon_box.w - S(24));
    const int field_top = Y(263), field_h = Y(342) - Y(263);                   // with the labels above
    hits_.push_back({{lat_box.x, field_top, lat_box.w + S(4), field_h}, FieldLat});
    hits_.push_back({{lon_box.x - S(4), field_top, lon_box.w + S(4), field_h}, FieldLon});

    double la, lo;
    const bool ready = installed_ && position(la, lo);
    std::string note = message_;
    if (note.empty()) note = !installed_ ? "viz1090 is not installed on this deck." : (!ready ? "Type an airport code, or enter the position." : "");
    text(note, S(14), kAmber, left, Y(347), column);

    const int buttons_y = Y(420), buttons_h = Y(52), hit_top = Y(400);
    const int back_w = static_cast<int>(std::lround((column - S(12)) * 130.0 / 292.0));
    const Rect back{left, buttons_y, back_w, buttons_h};
    const Rect go{left + back_w + S(12), buttons_y, column - back_w - S(12), buttons_h};
    button(back, "Back", S(28), kKey, kWhite, BtnBack, {back.x, hit_top, back.w + S(6), h_ - hit_top});
    button(go, "Start", S(32), ready ? kGold : SDL_Color{0x4A, 0x4A, 0x4A, 255}, kBlack, BtnStart,
           {go.x - S(6), hit_top, go.w + S(6), h_ - hit_top});

    // numeric keypad on the right
    struct Cell { int id; const char *label; int col, row, span; };
    static const Cell cells[] = {
        {K7, "7", 0, 0, 1}, {K8, "8", 1, 0, 1}, {K9, "9", 2, 0, 1},
        {K4, "4", 0, 1, 1}, {K5, "5", 1, 1, 1}, {K6, "6", 2, 1, 1},
        {K1, "1", 0, 2, 1}, {K2, "2", 1, 2, 1}, {K3, "3", 2, 2, 1},
        {KSign, "+/-", 0, 3, 1}, {K0, "0", 1, 3, 1}, {KDot, ".", 2, 3, 1},
        {KBack, "", 0, 4, 2}, {KNext, "Next", 2, 4, 1},
    };
    const int row_pitch = Y(80), key_h = Y(72);
    for (const Cell &c : cells) {
        const Rect r{keypad_x + c.col * key_pitch, Y(56) + c.row * row_pitch, c.span * key_w + (c.span - 1) * key_gap, key_h};
        const Rect hit{r.x - key_gap / 2, r.y - (row_pitch - key_h) / 2, r.w + key_gap, row_pitch};   // the gaps too
        const bool digit = c.id <= K9 || c.id == KDot || c.id == K0;
        button(r, c.label, c.id == KNext ? S(20) : S(32), digit ? kKey : kKey2, kWhite, c.id, hit);
        if (c.id == KBack) {                      // backspace symbol: an arrow-shaped key with a cross
            const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
            for (int t = 0; t < std::max(1, S(1)); ++t) {   // 1 px on the deck, thicker on bigger screens
                const Sint16 vx[5] = {(Sint16)(cx - S(22) + t), (Sint16)(cx - S(8)), (Sint16)(cx + S(22) - t),
                                      (Sint16)(cx + S(22) - t), (Sint16)(cx - S(8))};
                const Sint16 vy[5] = {(Sint16)cy, (Sint16)(cy - S(14) + t), (Sint16)(cy - S(14) + t),
                                      (Sint16)(cy + S(14) - t), (Sint16)(cy + S(14) - t)};
                polygonRGBA(renderer_, vx, vy, 5, 255, 255, 255, 255);
                lineRGBA(renderer_, cx - S(2) + t, cy - S(6), cx + S(12) + t, cy + S(6), 255, 255, 255, 255);
                lineRGBA(renderer_, cx - S(2) + t, cy + S(6), cx + S(12) + t, cy - S(6), 255, 255, 255, 255);
            }
        }
    }
    report_layout();
    SDL_RenderPresent(renderer_);
}

// Smaller than 640x480 (the 3.5" panel of the Pi 3A+): a plain notice and a way out.
void Setup::draw_notice()
{
    SDL_SetRenderDrawColor(renderer_, kBg.r, kBg.g, kBg.b, 255);
    SDL_RenderClear(renderer_);
    hits_.clear();
    const int body = std::max(16, std::min(w_, h_) / 16), margin = std::max(8, w_ / 40);
    text("viz1090", body * 7 / 5, kGold, margin, margin);
    text("viz1090 needs a 640x480 screen.", body, kWhite, margin, margin + body * 3, w_ - 2 * margin);
    char line[96];
    std::snprintf(line, sizeof(line), "This screen is %dx%d.", w_, h_);
    text(line, body, kMuted, margin, margin + body * 5, w_ - 2 * margin);
    const int bh = std::max(48, h_ / 6);
    const Rect back{margin, h_ - margin - bh, w_ - 2 * margin, bh};
    button(back, "Back", body * 7 / 5, kKey, kWhite, BtnBack, {0, back.y - margin, w_, h_ - back.y + margin});
    SDL_RenderPresent(renderer_);
}

// VIZ_LAYOUT_REPORT=1: every touch target once, with its size in mm (density of the screen), for the layout check.
void Setup::report_layout()
{
    if (reported_) return;
    reported_ = true;
    const char *v = std::getenv("VIZ_LAYOUT_REPORT");
    if (!v || !v[0] || v[0] == '0') return;
    const double ppmm = screen_.ppmm_x100 / 100.0;
    double smallest = 1e9;
    for (const Hit &hit : hits_) {
        const double wmm = hit.rect.w / ppmm, hmm = hit.rect.h / ppmm;
        smallest = std::min(smallest, std::min(wmm, hmm));
        const bool inside_screen = hit.rect.x >= 0 && hit.rect.y >= 0 && hit.rect.x + hit.rect.w <= w_ && hit.rect.y + hit.rect.h <= h_;
        std::fprintf(stderr, "[vizsetup] target %-16s %4d,%-4d %4dx%-4d px  %5.1f x %4.1f mm%s\n", id_name(hit.id),
                     hit.rect.x, hit.rect.y, hit.rect.w, hit.rect.h, wmm, hmm, inside_screen ? "" : "  OFF SCREEN");
    }
    std::fprintf(stderr, "[vizsetup] %dx%d at %.2f px/mm: smallest target side %.1f mm\n", w_, h_, ppmm, smallest);
}

void Setup::type_char(char ch)
{
    if (field_ == Field::Airport) field_ = Field::Lat;           // the keypad is for numbers
    std::string &value = field_ == Field::Lat ? lat_ : lon_;
    if (ch == '-') {
        if (!value.empty() && value[0] == '-') value.erase(0, 1);
        else value.insert(0, "-");
    } else if (ch == '.') {
        if (value.find('.') == std::string::npos) value += value.empty() || value == "-" ? "0." : ".";
    } else if (ch >= '0' && ch <= '9') {
        if (value.size() < 12) value += ch;
    }
    picked_.clear();
    message_.clear();
}

void Setup::type_airport(char ch)
{
    if (!std::isalpha(static_cast<unsigned char>(ch))) return;
    if (airport_.size() >= 3) airport_.clear();
    airport_ += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    picked_.clear();
    message_.clear();
    if (airport_.size() == 3)
        for (const Airport &airport : airports_)
            if (airport.code == airport_) {
                lat_ = degrees_text(airport.lat);
                lon_ = degrees_text(airport.lon);
                picked_ = airport.code;
                break;
            }
}

void Setup::backspace()
{
    std::string &value = field_ == Field::Airport ? airport_ : field_ == Field::Lat ? lat_ : lon_;
    if (!value.empty()) value.pop_back();
    picked_.clear();
    message_.clear();
}

void Setup::next_field(int step)
{
    int index = static_cast<int>(field_);
    index = (index + step + 3) % 3;
    field_ = static_cast<Field>(index);
}

void Setup::use_gps()
{
    if (!gps_fix_) return;
    lat_ = degrees_text(gps_lat_);
    lon_ = degrees_text(gps_lon_);
    picked_.clear();
    message_.clear();
}

void Setup::start()
{
    double lat = 0, lon = 0;
    if (too_small_ || !installed_ || !position(lat, lon)) return;
    if (!save_position(degrees_text(lat), degrees_text(lon), picked_)) {
        message_ = "Could not save the position.";
        return;
    }
    result_ = 0;
    done_ = true;
}

void Setup::activate(int id)
{
    if (id >= K0 && id <= K9) type_char(static_cast<char>('0' + id - K0));
    else if (id == KSign) type_char('-');
    else if (id == KDot) type_char('.');
    else if (id == KBack) backspace();
    else if (id == KNext) next_field(1);
    else if (id == FieldAirport) field_ = Field::Airport;
    else if (id == FieldLat) field_ = Field::Lat;
    else if (id == FieldLon) field_ = Field::Lon;
    else if (id == GpsPanel) use_gps();
    else if (id == BtnStart) start();
    else if (id == BtnBack) { result_ = 1; done_ = true; }
}

int hit_id(const std::vector<Hit> &hits, int x, int y)
{
    for (auto it = hits.rbegin(); it != hits.rend(); ++it)
        if (it->rect.contains(x, y)) return it->id;
    return None;
}

void Setup::handle(const SDL_Event &event)
{
    switch (event.type) {
    case SDL_QUIT:
        result_ = 1;
        done_ = true;
        break;
    case SDL_FINGERDOWN:       // finger positions are 0..1 of the window
        pressed_ = hit_id(hits_, static_cast<int>(event.tfinger.x * w_), static_cast<int>(event.tfinger.y * h_));
        break;
    case SDL_FINGERUP: {
        const int id = hit_id(hits_, static_cast<int>(event.tfinger.x * w_), static_cast<int>(event.tfinger.y * h_));
        if (id != None && id == pressed_) activate(id);
        pressed_ = None;
        break;
    }
    case SDL_KEYDOWN:
        switch (event.key.keysym.sym) {
        case SDLK_ESCAPE: result_ = 1; done_ = true; break;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            if (too_small_) { result_ = 1; done_ = true; }
            else start();
            break;
        case SDLK_TAB: case SDLK_DOWN: next_field(1); break;
        case SDLK_UP: next_field(-1); break;
        case SDLK_BACKSPACE: backspace(); break;
        default: break;
        }
        break;
    case SDL_TEXTINPUT: {
        if (too_small_) break;
        const char ch = event.text.text[0];
        if (field_ == Field::Airport) {
            if (std::isalpha(static_cast<unsigned char>(ch))) type_airport(ch);
            else if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') type_char(ch);   // digits mean latitude / longitude
        } else if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
            type_char(ch);
        } else if (ch == ',') {
            next_field(1);
        }
        break;
    }
    default:
        break;
    }
}

int Setup::run()
{
    viz_screen_detect(&screen_);
    w_ = screen_.w;
    h_ = screen_.h;
    sx_ = w_ / 640.0f;
    sy_ = h_ / 480.0f;
    s_ = std::min(sx_, sy_);
    too_small_ = w_ < 640 || h_ < 480;
    std::fprintf(stderr, "[vizsetup] screen %dx%d (%s), %d.%02d px/mm%s\n", w_, h_, screen_.source,
                 screen_.ppmm_x100 / 100, screen_.ppmm_x100 % 100, too_small_ ? ": too small, notice only" : "");

    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) return 2;
    window_ = SDL_CreateWindow("viz1090", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w_, h_, 0);
    if (!window_) return 2;
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) return 2;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    if (!too_small_) {
        const char *program = std::getenv("VIZ_PROGRAM");
        installed_ = access(program && program[0] ? program : "/opt/viz1090/viz1090", X_OK) == 0;
        load_position(lat_, lon_, airport_);
        airports_ = load_airports();
        picked_ = airport_;
        std::thread(gps_worker, gps_).detach();
    }

    while (!done_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) handle(event);
        if (too_small_) draw_notice();
        else draw();
        SDL_Delay(50);
    }
    gps_->stop = true;
    clear_cache();
    TTF_Quit();
    SDL_Quit();
    return result_;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--screen") == 0) {   // for run_viz1090.sh: the screen and viz1090's uiscale
        viz_screen_t screen;
        viz_screen_detect(&screen);
        std::printf("%d %d %d %d %s\n", screen.w, screen.h, viz_screen_uiscale(&screen), screen.ppmm_x100, screen.source);
        return 0;
    }
    Setup setup;
    return setup.run();
}
