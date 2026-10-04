// SPDX-License-Identifier: MIT
//
// vizsetup: the intro screen of viz1090 on the cyberdeck (640x480, touch and keyboard).
//
// Shows whether the RTL-SDR dongle and a GPS receiver are connected, takes the position (the code of the
// nearest airport typed on the keyboard, the GPS, or latitude / longitude on a touch keypad), saves it to
// ~/.config/cardputerzero/viz1090.conf and exits with 0 (start viz1090) or 1 (cancelled).
//
// Runs through the same display / input bridge as viz1090 (libviz_fb.so, SDL_VIDEODRIVER=offscreen).
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

    TTF_Font *font(int px);
    void text(const std::string &value, int px, SDL_Color color, int x, int y, int max_width = 0, bool right = false);
    int text_width(const std::string &value, int px);
    void panel(Rect r, SDL_Color color, int radius = 12);
    void button(Rect r, const std::string &label, int px, SDL_Color color, SDL_Color text_color, int id);
    void draw();
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

void Setup::button(Rect r, const std::string &label, int px, SDL_Color color, SDL_Color text_color, int id)
{
    if (pressed_ == id) color = SDL_Color{0x70, 0x70, 0x70, 255};
    panel(r, color, 14);
    const int w = text_width(label, px);
    const int h = font(px) ? TTF_FontHeight(font(px)) : px;
    text(label, px, text_color, r.x + (r.w - w) / 2, r.y + (r.h - h) / 2);
    hits_.push_back({r, id});
}

bool Setup::position(double &lat, double &lon) const
{
    return parse_degrees(lat_, 90.0, lat) && parse_degrees(lon_, 180.0, lon);
}

void Setup::draw()
{
    SDL_SetRenderDrawColor(renderer_, kBg.r, kBg.g, kBg.b, 255);
    SDL_RenderClear(renderer_);
    hits_.clear();

    text("viz1090", 28, kGold, 16, 6);
    // SDR
    static std::string sdr;
    static Uint32 last_scan = 0;
    if (SDL_GetTicks() - last_scan > 1000 || last_scan == 0) { sdr = find_sdr(); last_scan = SDL_GetTicks(); }
    panel({16, 52, 304, 56}, kPanel);
    filledCircleRGBA(renderer_, 32, 80, 9, sdr.empty() ? kRed.r : kGreen.r, sdr.empty() ? kRed.g : kGreen.g, sdr.empty() ? kRed.b : kGreen.b, 255);
    text("SDR receiver", 14, kMuted, 52, 55);
    text(sdr.empty() ? "Not found: plug it in" : sdr, 20, sdr.empty() ? kRed : kWhite, 52, 74, 256);

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
    panel({16, 114, 304, 56}, kPanel);
    SDL_Color gps_color = state == 2 ? kGreen : state == 1 ? kAmber : kMuted;
    filledCircleRGBA(renderer_, 32, 142, 9, gps_color.r, gps_color.g, gps_color.b, 255);
    text("GPS receiver (tap to use)", 14, kMuted, 52, 117);
    char line[96];
    if (state == 0) std::snprintf(line, sizeof(line), "None detected");
    else if (state == 1) std::snprintf(line, sizeof(line), "%s: waiting for a fix", name.empty() ? port.c_str() : name.c_str());
    else std::snprintf(line, sizeof(line), "Fix %.4f, %.4f (%d sats)", gps_lat_, gps_lon_, sats);
    text(line, 20, state == 0 ? kMuted : state == 1 ? kAmber : kWhite, 52, 136, 256);
    hits_.push_back({{16, 114, 304, 56}, GpsPanel});

    // airport code
    text("Nearest airport code", 14, kMuted, 16, 179);
    panel({16, 200, 150, 56}, kPanel);
    if (field_ == Field::Airport) rectangleRGBA(renderer_, 16, 200, 165, 255, kBlue.r, kBlue.g, kBlue.b, 255);
    if (field_ == Field::Airport) rectangleRGBA(renderer_, 17, 201, 164, 254, kBlue.r, kBlue.g, kBlue.b, 255);
    if (field_ == Field::Airport) rectangleRGBA(renderer_, 18, 202, 163, 253, kBlue.r, kBlue.g, kBlue.b, 255);
    text(airport_.empty() ? "CDG" : airport_, 32, airport_.empty() ? kDim : kWhite, 30, 208);
    hits_.push_back({{16, 200, 150, 56}, FieldAirport});
    if (airports_.empty()) text("no airport list", 20, kMuted, 178, 216);
    else if (!picked_.empty() && picked_ == airport_) text("found", 20, kGreen, 178, 216);
    else if (airport_.size() >= 3) text("unknown code", 20, kRed, 178, 216);

    // latitude / longitude
    text("Latitude", 14, kMuted, 16, 267);
    text("Longitude", 14, kMuted, 172, 267);
    const Rect lat_box{16, 288, 148, 50}, lon_box{172, 288, 148, 50};
    panel(lat_box, kPanel);
    panel(lon_box, kPanel);
    for (int i = 0; i < 3; ++i) {
        if (field_ == Field::Lat) rectangleRGBA(renderer_, lat_box.x + i, lat_box.y + i, lat_box.x + lat_box.w - 1 - i, lat_box.y + lat_box.h - 1 - i, kBlue.r, kBlue.g, kBlue.b, 255);
        if (field_ == Field::Lon) rectangleRGBA(renderer_, lon_box.x + i, lon_box.y + i, lon_box.x + lon_box.w - 1 - i, lon_box.y + lon_box.h - 1 - i, kBlue.r, kBlue.g, kBlue.b, 255);
    }
    text(lat_.empty() ? "--" : lat_, 20, lat_.empty() ? kDim : kWhite, lat_box.x + 12, lat_box.y + 12, 124);
    text(lon_.empty() ? "--" : lon_, 20, lon_.empty() ? kDim : kWhite, lon_box.x + 12, lon_box.y + 12, 124);
    hits_.push_back({lat_box, FieldLat});
    hits_.push_back({lon_box, FieldLon});

    double la, lo;
    const bool ready = installed_ && position(la, lo);
    std::string note = message_;
    if (note.empty()) note = !installed_ ? "viz1090 is not installed on this deck." : (!ready ? "Type an airport code, or enter the position." : "");
    text(note, 14, kAmber, 16, 347, 304);

    button({16, 420, 130, 52}, "Back", 28, kKey, kWhite, BtnBack);
    button({158, 420, 162, 52}, "Start", 32, ready ? kGold : SDL_Color{0x4A, 0x4A, 0x4A, 255}, kBlack, BtnStart);

    // numeric keypad on the right
    struct Cell { int id; const char *label; int col, row, span; };
    static const Cell cells[] = {
        {K7, "7", 0, 0, 1}, {K8, "8", 1, 0, 1}, {K9, "9", 2, 0, 1},
        {K4, "4", 0, 1, 1}, {K5, "5", 1, 1, 1}, {K6, "6", 2, 1, 1},
        {K1, "1", 0, 2, 1}, {K2, "2", 1, 2, 1}, {K3, "3", 2, 2, 1},
        {KSign, "+/-", 0, 3, 1}, {K0, "0", 1, 3, 1}, {KDot, ".", 2, 3, 1},
        {KBack, "", 0, 4, 2}, {KNext, "Next", 2, 4, 1},
    };
    for (const Cell &c : cells) {
        const Rect r{340 + c.col * 96, 56 + c.row * 80, c.span * 88 + (c.span - 1) * 8, 72};
        const bool digit = c.id <= K9 || c.id == KDot || c.id == K0;
        button(r, c.label, c.id == KNext ? 20 : 32, digit ? kKey : kKey2, kWhite, c.id);
        if (c.id == KBack) {                      // backspace symbol: an arrow-shaped key with a cross
            const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
            const Sint16 vx[5] = {(Sint16)(cx - 22), (Sint16)(cx - 8), (Sint16)(cx + 22), (Sint16)(cx + 22), (Sint16)(cx - 8)};
            const Sint16 vy[5] = {(Sint16)cy, (Sint16)(cy - 14), (Sint16)(cy - 14), (Sint16)(cy + 14), (Sint16)(cy + 14)};
            polygonRGBA(renderer_, vx, vy, 5, 255, 255, 255, 255);
            lineRGBA(renderer_, cx - 2, cy - 6, cx + 12, cy + 6, 255, 255, 255, 255);
            lineRGBA(renderer_, cx - 2, cy + 6, cx + 12, cy - 6, 255, 255, 255, 255);
        }
    }
    SDL_RenderPresent(renderer_);
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
    if (!installed_ || !position(lat, lon)) return;
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
    case SDL_FINGERDOWN:
        pressed_ = hit_id(hits_, static_cast<int>(event.tfinger.x * 640), static_cast<int>(event.tfinger.y * 480));
        break;
    case SDL_FINGERUP: {
        const int id = hit_id(hits_, static_cast<int>(event.tfinger.x * 640), static_cast<int>(event.tfinger.y * 480));
        if (id != None && id == pressed_) activate(id);
        pressed_ = None;
        break;
    }
    case SDL_KEYDOWN:
        switch (event.key.keysym.sym) {
        case SDLK_ESCAPE: result_ = 1; done_ = true; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: start(); break;
        case SDLK_TAB: case SDLK_DOWN: next_field(1); break;
        case SDLK_UP: next_field(-1); break;
        case SDLK_BACKSPACE: backspace(); break;
        default: break;
        }
        break;
    case SDL_TEXTINPUT: {
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
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) return 2;
    window_ = SDL_CreateWindow("viz1090", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, 0);
    if (!window_) return 2;
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) return 2;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    installed_ = access("/opt/viz1090/viz1090", X_OK) == 0;
    load_position(lat_, lon_, airport_);
    airports_ = load_airports();
    picked_ = airport_;
    std::thread(gps_worker, gps_).detach();

    while (!done_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) handle(event);
        draw();
        SDL_Delay(50);
    }
    gps_->stop = true;
    clear_cache();
    TTF_Quit();
    SDL_Quit();
    return result_;
}

} // namespace

int main()
{
    Setup setup;
    return setup.run();
}
