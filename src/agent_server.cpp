#include "agent_server.h"
#include "window_callbacks.h"
#include "jni/jni_support.h"
#include "fake_egl.h"
#include "frame_pacer.h"

#include <game_window.h>
#include <log.h>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::shared_ptr<GameWindow> window;
std::shared_ptr<WindowCallbacks> callbacks;
JniSupport *jni = nullptr;

struct QueuedEvent {
    Clock::time_point at;
    std::function<void()> fn;
};
std::mutex queueMutex;
std::deque<QueuedEvent> queue;
double lastMouseX = 0, lastMouseY = 0;

std::mutex captureMutex;
std::condition_variable captureCv;
std::atomic<bool> captureRequested{false};
bool captureReady = false;
int captureWidth = 0, captureHeight = 0;
std::vector<uint8_t> capturePixels;  // RGBA, bottom row first

using PFN_glReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void *);
using PFN_glGetIntegerv = void (*)(unsigned, int *);
using PFN_glBindFramebuffer = void (*)(unsigned, unsigned);
using PFN_glPixelStorei = void (*)(unsigned, int);
constexpr unsigned GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401, GL_READ_FRAMEBUFFER_ = 0x8CA8,
                   GL_READ_FRAMEBUFFER_BINDING_ = 0x8CAA, GL_PACK_ALIGNMENT_ = 0x0D05;

void enqueue(std::chrono::milliseconds delay, std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(queueMutex);
    queue.push_back({Clock::now() + delay, std::move(fn)});
}

KeyCode keyFromName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    if(name.size() == 1) {
        char c = name[0];
        if(c >= 'a' && c <= 'z')
            return (KeyCode)((int)KeyCode::A + (c - 'a'));
        if(c >= '0' && c <= '9')
            return (KeyCode)((int)KeyCode::NUM_0 + (c - '0'));
        if(c == ' ')
            return KeyCode::SPACE;
    }
    if(name.size() >= 2 && name[0] == 'f' && isdigit((unsigned char)name[1])) {
        int n = atoi(name.c_str() + 1);
        if(n >= 1 && n <= 12)
            return (KeyCode)((int)KeyCode::FN1 + n - 1);
    }
    static const std::unordered_map<std::string, KeyCode> named = {
        {"space", KeyCode::SPACE}, {"enter", KeyCode::ENTER}, {"return", KeyCode::ENTER},
        {"escape", KeyCode::ESCAPE}, {"esc", KeyCode::ESCAPE}, {"tab", KeyCode::TAB},
        {"backspace", KeyCode::BACKSPACE}, {"delete", KeyCode::DELETE}, {"insert", KeyCode::INSERT},
        {"shift", KeyCode::LEFT_SHIFT}, {"lshift", KeyCode::LEFT_SHIFT}, {"rshift", KeyCode::RIGHT_SHIFT},
        {"ctrl", KeyCode::LEFT_CTRL}, {"lctrl", KeyCode::LEFT_CTRL}, {"rctrl", KeyCode::RIGHT_CTRL},
        {"alt", KeyCode::LEFT_ALT}, {"lalt", KeyCode::LEFT_ALT}, {"ralt", KeyCode::RIGHT_ALT},
        {"super", KeyCode::LEFT_SUPER}, {"cmd", KeyCode::LEFT_SUPER}, {"meta", KeyCode::LEFT_SUPER},
        {"up", KeyCode::UP}, {"down", KeyCode::DOWN}, {"left", KeyCode::LEFT}, {"right", KeyCode::RIGHT},
        {"home", KeyCode::HOME}, {"end", KeyCode::END}, {"pageup", KeyCode::PAGE_UP}, {"pagedown", KeyCode::PAGE_DOWN},
        {"capslock", KeyCode::CAPS_LOCK}, {"pause", KeyCode::PAUSE},
        {"comma", KeyCode::COMMA}, {"period", KeyCode::PERIOD}, {"slash", KeyCode::SLASH},
        {"semicolon", KeyCode::SEMICOLON}, {"apostrophe", KeyCode::APOSTROPHE}, {"minus", KeyCode::MINUS},
        {"equal", KeyCode::EQUAL}, {"grave", KeyCode::GRAVE}, {"lbracket", KeyCode::LEFT_BRACKET},
        {"rbracket", KeyCode::RIGHT_BRACKET}, {"backslash", KeyCode::BACKSLASH},
    };
    auto it = named.find(name);
    return it == named.end() ? KeyCode::UNKNOWN : it->second;
}

int modsFromJson(json const &req) {
    int mods = 0;
    if(!req.contains("mods") || !req["mods"].is_array())
        return mods;
    for(auto &m : req["mods"]) {
        std::string s = m.get<std::string>();
        if(s == "shift") mods |= KEY_MOD_SHIFT;
        else if(s == "ctrl") mods |= KEY_MOD_CTRL;
        else if(s == "alt") mods |= KEY_MOD_ALT;
        else if(s == "super" || s == "cmd") mods |= KEY_MOD_SUPER;
    }
    return mods;
}

int buttonFromJson(json const &req) {
    if(!req.contains("button"))
        return 1;
    if(req["button"].is_number())
        return req["button"].get<int>();
    std::string b = req["button"].get<std::string>();
    if(b == "right") return 2;
    if(b == "middle") return 3;
    return 1;
}

std::string base64(std::vector<uint8_t> const &in) {
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for(; i + 2 < in.size(); i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if(i + 1 == in.size()) {
        uint32_t v = in[i] << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if(i + 2 == in.size()) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

void appendChunk(std::vector<uint8_t> &png, const char *type, std::vector<uint8_t> const &data) {
    auto be32 = [&](uint32_t v) { png.push_back(v >> 24); png.push_back(v >> 16); png.push_back(v >> 8); png.push_back(v); };
    be32((uint32_t)data.size());
    size_t crcStart = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    be32((uint32_t)crc32(0, png.data() + crcStart, (uInt)(png.size() - crcStart)));
}

// Box-filters the bottom-up RGBA capture to (w, h), flips it upright and encodes RGB PNG.
std::vector<uint8_t> encodePng(std::vector<uint8_t> const &rgba, int srcW, int srcH, int w, int h) {
    std::vector<uint8_t> raw((size_t)h * (1 + (size_t)w * 3));
    for(int y = 0; y < h; y++) {
        uint8_t *row = raw.data() + (size_t)y * (1 + (size_t)w * 3);
        *row++ = 0;
        int sy0 = (h - 1 - y) * srcH / h, sy1 = std::max(sy0 + 1, (h - y) * srcH / h);
        for(int x = 0; x < w; x++) {
            int sx0 = x * srcW / w, sx1 = std::max(sx0 + 1, (x + 1) * srcW / w);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for(int sy = sy0; sy < sy1; sy++) {
                const uint8_t *p = rgba.data() + ((size_t)sy * srcW + sx0) * 4;
                for(int sx = sx0; sx < sx1; sx++, p += 4) {
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            }
            *row++ = r / n; *row++ = g / n; *row++ = b / n;
        }
    }
    std::vector<uint8_t> z(compressBound((uLong)raw.size()));
    uLongf zlen = (uLongf)z.size();
    compress2(z.data(), &zlen, raw.data(), (uLong)raw.size(), 6);
    z.resize(zlen);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr = {
        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
        8, 2, 0, 0, 0};
    appendChunk(png, "IHDR", ihdr);
    appendChunk(png, "IDAT", z);
    appendChunk(png, "IEND", {});
    return png;
}

json screenshot(json const &req) {
    std::unique_lock<std::mutex> lock(captureMutex);
    captureReady = false;
    captureRequested = true;
    if(!captureCv.wait_for(lock, std::chrono::seconds(3), [] { return captureReady; })) {
        captureRequested = false;
        return {{"ok", false}, {"error", "no frame rendered within 3s"}};
    }
    int srcW = captureWidth, srcH = captureHeight;
    int w = req.value("width", 0), h = req.value("height", 0);
    if(w <= 0 && h <= 0) {
        w = srcW; h = srcH;
    } else if(w <= 0) {
        w = std::max(1, srcW * h / srcH);
    } else if(h <= 0) {
        h = std::max(1, srcH * w / srcW);
    }
    w = std::min(w, srcW); h = std::min(h, srcH);
    auto png = encodePng(capturePixels, srcW, srcH, w, h);
    return {{"ok", true}, {"width", w}, {"height", h}, {"source_width", srcW}, {"source_height", srcH}, {"png_base64", base64(png)}};
}

json handle(json const &req) {
    std::string cmd = req.value("cmd", "");
    if(cmd == "ping")
        return {{"ok", true}};
    if(cmd == "state") {
        int w = 0, h = 0;
        window->getWindowSize(w, h);
        return {{"ok", true}, {"width", w}, {"height", h}, {"focused", window->isFocused()},
                {"cursor_locked", window->getCursorDisabled()}, {"fps", FramePacer::measuredFps()},
                {"fps_cap", FramePacer::activeCap(window.get())}, {"mouse_x", lastMouseX}, {"mouse_y", lastMouseY}};
    }
    if(cmd == "key") {
        auto key = keyFromName(req.value("key", ""));
        if(key == KeyCode::UNKNOWN)
            return {{"ok", false}, {"error", "unknown key"}};
        std::string action = req.value("action", "tap");
        int mods = modsFromJson(req);
        auto hold = std::chrono::milliseconds(req.value("hold_ms", 60));
        if(action == "press" || action == "tap")
            enqueue(std::chrono::milliseconds(0), [key, mods] { callbacks->onKeyboard(key, KeyAction::PRESS, mods); });
        if(action == "release" || action == "tap")
            enqueue(action == "tap" ? hold : std::chrono::milliseconds(0), [key, mods] { callbacks->onKeyboard(key, KeyAction::RELEASE, mods); });
        return {{"ok", true}};
    }
    if(cmd == "text") {
        std::string text = req.value("text", "");
        enqueue(std::chrono::milliseconds(0), [text] { callbacks->onKeyboardText(text); });
        return {{"ok", true}};
    }
    if(cmd == "mouse_move") {
        double dx = req.value("dx", 0.0), dy = req.value("dy", 0.0);
        enqueue(std::chrono::milliseconds(0), [dx, dy] { callbacks->onMouseRelativePosition(dx, dy); });
        return {{"ok", true}};
    }
    if(cmd == "mouse_pos") {
        lastMouseX = req.value("x", lastMouseX);
        lastMouseY = req.value("y", lastMouseY);
        double x = lastMouseX, y = lastMouseY;
        enqueue(std::chrono::milliseconds(0), [x, y] { callbacks->onMousePosition(x, y); });
        return {{"ok", true}};
    }
    if(cmd == "click") {
        if(req.contains("x") && req.contains("y")) {
            lastMouseX = req["x"].get<double>();
            lastMouseY = req["y"].get<double>();
            double x = lastMouseX, y = lastMouseY;
            enqueue(std::chrono::milliseconds(0), [x, y] { callbacks->onMousePosition(x, y); });
        }
        int btn = buttonFromJson(req);
        std::string action = req.value("action", "tap");
        auto hold = std::chrono::milliseconds(req.value("hold_ms", 60));
        double x = lastMouseX, y = lastMouseY;
        if(action == "press" || action == "tap")
            enqueue(std::chrono::milliseconds(0), [x, y, btn] { callbacks->onMouseButton(x, y, btn, MouseButtonAction::PRESS); });
        if(action == "release" || action == "tap")
            enqueue(action == "tap" ? hold : std::chrono::milliseconds(0), [x, y, btn] { callbacks->onMouseButton(x, y, btn, MouseButtonAction::RELEASE); });
        return {{"ok", true}};
    }
    if(cmd == "scroll") {
        double dx = req.value("dx", 0.0), dy = req.value("dy", 0.0), x = lastMouseX, y = lastMouseY;
        enqueue(std::chrono::milliseconds(0), [x, y, dx, dy] { callbacks->onMouseScroll(x, y, dx, dy); });
        return {{"ok", true}};
    }
    if(cmd == "screenshot")
        return screenshot(req);
    if(cmd == "fps") {
        FramePacer::setCap(req.value("cap", 0));
        return {{"ok", true}, {"fps_cap", FramePacer::activeCap(window.get())}};
    }
    if(cmd == "quit") {
        jni->requestExitGame();
        return {{"ok", true}};
    }
    return {{"ok", false}, {"error", "unknown cmd"}};
}

bool sendAll(int fd, std::string const &s) {
    size_t off = 0;
    while(off < s.size()) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
#endif
        if(n <= 0)
            return false;
        off += (size_t)n;
    }
    return true;
}

void serveClient(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    std::string buf;
    char chunk[4096];
    for(;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if(n <= 0)
            break;
        buf.append(chunk, (size_t)n);
        size_t nl;
        while((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if(line.empty())
                continue;
            json res;
            try {
                auto req = json::parse(line);
                res = handle(req);
                if(req.contains("id"))
                    res["id"] = req["id"];
            } catch(std::exception const &e) {
                res = {{"ok", false}, {"error", e.what()}};
            }
            if(!sendAll(fd, res.dump() + "\n"))
                break;
        }
    }
    close(fd);
}

}  // namespace

void AgentServer::start(std::string const &path, std::shared_ptr<GameWindow> w, std::shared_ptr<WindowCallbacks> cb, JniSupport *j) {
    window = std::move(w);
    callbacks = std::move(cb);
    jni = j;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd < 0) {
        Log::error("AgentServer", "socket() failed: %s", strerror(errno));
        return;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if(path.size() >= sizeof(addr.sun_path)) {
        Log::error("AgentServer", "socket path too long: %s", path.c_str());
        close(fd);
        return;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    unlink(path.c_str());
    if(bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        Log::error("AgentServer", "bind/listen on %s failed: %s", path.c_str(), strerror(errno));
        close(fd);
        return;
    }
    Log::info("AgentServer", "Listening on %s", path.c_str());
    std::thread([fd] {
        for(;;) {
            int client = accept(fd, nullptr, nullptr);
            if(client < 0)
                continue;
            std::thread(serveClient, client).detach();
        }
    }).detach();
}

void AgentServer::drain() {
    if(!callbacks)
        return;
    std::vector<std::function<void()>> due;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        auto now = Clock::now();
        for(auto it = queue.begin(); it != queue.end();) {
            if(it->at <= now) {
                due.push_back(std::move(it->fn));
                it = queue.erase(it);
            } else {
                ++it;
            }
        }
    }
    for(auto &fn : due)
        fn();
}

void AgentServer::onBeforeSwap(GameWindow *w) {
    if(!captureRequested.load())
        return;
    static auto glReadPixels = (PFN_glReadPixels)fake_egl::eglGetProcAddress("glReadPixels");
    static auto glGetIntegerv = (PFN_glGetIntegerv)fake_egl::eglGetProcAddress("glGetIntegerv");
    static auto glBindFramebuffer = (PFN_glBindFramebuffer)fake_egl::eglGetProcAddress("glBindFramebuffer");
    static auto glPixelStorei = (PFN_glPixelStorei)fake_egl::eglGetProcAddress("glPixelStorei");
    if(!glReadPixels || !glGetIntegerv || !glBindFramebuffer || !glPixelStorei) {
        captureRequested = false;
        return;
    }
    int width = 0, height = 0;
    w->getWindowSize(width, height);
    if(width <= 0 || height <= 0)
        return;
    std::lock_guard<std::mutex> lock(captureMutex);
    int prevFbo = 0, prevAlign = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_, &prevFbo);
    glGetIntegerv(GL_PACK_ALIGNMENT_, &prevAlign);
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    glPixelStorei(GL_PACK_ALIGNMENT_, 1);
    capturePixels.resize((size_t)width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA_, GL_UNSIGNED_BYTE_, capturePixels.data());
    glPixelStorei(GL_PACK_ALIGNMENT_, prevAlign);
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, (unsigned)prevFbo);
    captureWidth = width;
    captureHeight = height;
    captureRequested = false;
    captureReady = true;
    captureCv.notify_all();
}
