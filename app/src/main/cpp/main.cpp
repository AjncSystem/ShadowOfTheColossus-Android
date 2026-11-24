// SPDX-License-Identifier: GPL-2.0-or-later
// SotC-Android, milestone M0: proves the platform layer the port will sit on.
//  - SDL3 owns the window, input and the native main thread;
//  - the GPU driver (Turnip Gen8 V38 by default) is loaded through libadrenotools;
//  - a Vulkan swapchain clears the screen; gamepad buttons and sticks change the color;
//  - a report of the driver's features/extensions vs. what shadPS4 needs is written to
//    /sdcard/Android/data/io.github.ajncsystem.sotc/files/m0-report.txt and to logcat (tag SotC).
#include <cmath>
#include <cstdio>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <pthread.h>
#include <sys/stat.h>

#include "gpu_driver.h"
#include "guest/m1_selftest.h"
#include <unistd.h>

#include "loader/m3_run.h"
#include "log.h"
#include "vk_smoke.h"

namespace {

struct Args {
    GpuDriverConfig gpu;
    std::string files_dir;
    std::string external_dir;
    std::string app0;  // the game dump, found by GameLocator.java
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        std::string v = argv[i + 1];
        if (k == "--native-lib-dir") a.gpu.native_lib_dir = v;
        else if (k == "--driver-dir") a.gpu.driver_dir = v;
        else if (k == "--driver-lib") a.gpu.driver_lib = v;
        else if (k == "--driver-name") a.gpu.driver_name = v;
        else if (k == "--files-dir") a.files_dir = v;
        else if (k == "--external-dir") a.external_dir = v;
        else if (k == "--app0") a.app0 = v;
        else LOGW("unknown argument %s", k.c_str());
    }
    return a;
}

void WriteText(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        LOGW("cannot write %s", path.c_str());
        return;
    }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

void LogLines(const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        LOGI("%.*s", static_cast<int>(end - start), text.data() + start);
        start = end + 1;
    }
}

// The SotC-PC runtime reports through printf/stderr: send both to logcat (tag SotC-RT).
void* StdioPump(void* arg) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    char buf[2048];
    std::string pending;
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        pending.append(buf, static_cast<size_t>(n));
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            __android_log_print(ANDROID_LOG_INFO, "SotC-RT", "%.*s", static_cast<int>(nl), pending.data());
            pending.erase(0, nl + 1);
        }
    }
    return nullptr;
}

void RedirectStdio() {
    static bool done = false;
    if (done) return;
    done = true;
    int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    pthread_t t;
    if (pthread_create(&t, nullptr, StdioPump, reinterpret_cast<void*>(static_cast<intptr_t>(fds[0]))) == 0) {
        pthread_setname_np(t, "stdio-logcat");
        pthread_detach(t);
    }
}

// The game runs on its own host thread: FEX compiles on the caller's stack, and the guest main
// thread blocks for the life of the game.
struct GameJob {
    sotc::loader::M3Paths paths;
    std::string report_path;
};

void* GameThread(void* arg) {
    auto* job = static_cast<GameJob*>(arg);
    std::string report = "SotC-Android M3 report\n";
    sotc::loader::RunM3(job->paths, &report);
    WriteText(job->report_path, report);
    delete job;
    return nullptr;
}

void StartGame(const std::string& external_dir, const std::string& app0) {
    const std::string game = external_dir + "/game";
    auto* job = new GameJob{{game + "/boot-linked.bin", game + "/content.bin", app0, external_dir + "/user"},
                            external_dir + "/m3-report.txt"};
    struct stat st;
    if (stat(job->paths.boot_image.c_str(), &st) != 0) {
        LOGI("game skipped: %s not found (adb push SotC-PC's out/boot-linked.bin there)", job->paths.boot_image.c_str());
        delete job;
        return;
    }
    if (app0.empty() || stat((app0 + "/eboot.bin").c_str(), &st) != 0) {
        LOGI("game skipped: no game dump found (allow All files access and keep CUSA08809 under \"PlayStation 4\")");
        delete job;
        return;
    }
    RedirectStdio();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attr, GameThread, job) == 0) {
        pthread_setname_np(thread, "guest-main");
        pthread_detach(thread);
    } else {
        delete job;
    }
    pthread_attr_destroy(&attr);
}

ANativeWindow* CurrentNativeWindow(SDL_Window* window) {
    return static_cast<ANativeWindow*>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args = ParseArgs(argc, argv);
    LOGI("SotC-Android M0 starting");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOGE("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("SotC", 0, 0, SDL_WINDOW_FULLSCREEN);
    if (!window) {
        LOGE("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }

    std::string used;
    PFN_vkGetInstanceProcAddr gipa = LoadVulkanDriver(args.gpu, &used);
    VkSmoke vk;
    std::string report = "SotC-Android M0 report\nGPU driver loaded: " + used + "\n";
    bool ok = gipa && vk.Init(gipa);
    report += ok ? vk.Report() : std::string("Vulkan init FAILED\n");
    LogLines(report);
    if (!args.external_dir.empty()) WriteText(args.external_dir + "/m0-report.txt", report);
    if (!ok) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SotC", "Vulkan init failed, see logcat (tag SotC).", window);
        SDL_Quit();
        return 1;
    }

    vk.CreateSurface(CurrentNativeWindow(window));

    // M1: guest x86-64 execution through FEXCore. Screen tint: green = pass, red = fail.
    std::string m1 = "SotC-Android M1 report\n";
    const bool m1_ok = sotc::guest::RunM1SelfTest(args.files_dir, &m1);
    LogLines(m1);
    if (!args.external_dir.empty()) WriteText(args.external_dir + "/m1-report.txt", m1);
    const float tint[3] = {m1_ok ? 0.0f : 0.6f, m1_ok ? 0.45f : 0.0f, 0.0f};
    // The game's presenter takes the window: release the test swapchain first (one swapchain
    // per surface) and stop drawing; this loop keeps pumping SDL events for the game.
    bool game_owns_window = false;
    if (m1_ok && !args.external_dir.empty() && !args.app0.empty()) {
        vk.DestroySurface();
        game_owns_window = true;
        StartGame(args.external_dir, args.app0);
    }

    bool running = true;
    bool paused = false;
    float hue = 0.0f;
    float boost[3] = {0, 0, 0};
    float stick_x = 0.0f, stick_y = 0.0f;
    uint64_t frames = 0;
    uint64_t last_report = SDL_GetTicksNS();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING:
                running = false;
                break;
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                paused = true;
                vk.DestroySurface();
                LOGI("paused (surface released)");
                break;
            case SDL_EVENT_DID_ENTER_FOREGROUND:
                paused = false;
                if (!game_owns_window && !vk.HasSurface()) vk.CreateSurface(CurrentNativeWindow(window));
                LOGI("resumed");
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (!game_owns_window && vk.HasSurface()) vk.RecreateSwapchain();
                break;
            case SDL_EVENT_GAMEPAD_ADDED: {
                SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which);
                LOGI("gamepad added: %s (type %d)", pad ? SDL_GetGamepadName(pad) : "?",
                     pad ? SDL_GetGamepadType(pad) : -1);
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED:
                LOGI("gamepad removed");
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                const auto b = static_cast<SDL_GamepadButton>(e.gbutton.button);
                LOGI("button %s", SDL_GetGamepadStringForButton(b));
                if (b == SDL_GAMEPAD_BUTTON_SOUTH) boost[2] = 1.0f;
                if (b == SDL_GAMEPAD_BUTTON_EAST) boost[0] = 1.0f;
                if (b == SDL_GAMEPAD_BUTTON_NORTH) boost[1] = 1.0f;
                if (b == SDL_GAMEPAD_BUTTON_WEST) boost[0] = boost[2] = 1.0f;
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                const float v = e.gaxis.value / 32767.0f;
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) stick_x = v;
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) stick_y = v;
                if ((e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) && v > 0.5f)
                    LOGI("trigger %s", SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(e.gaxis.axis)));
                break;
            }
            case SDL_EVENT_KEY_DOWN:
                LOGI("key %s", SDL_GetKeyName(e.key.key));
                if (e.key.key == SDLK_AC_BACK) running = false;
                break;
            default:
                break;
            }
        }
        if (!running) break;
        if (paused || !vk.HasSurface()) {
            SDL_Delay(16);
            continue;
        }

        hue += 0.004f + stick_x * 0.02f;
        const float light = 0.35f - stick_y * 0.25f;
        float rgb[3];
        for (int c = 0; c < 3; ++c) {
            rgb[c] = tint[c] + light * 0.5f + 0.15f * std::sin(hue * 6.2831853f + c * 2.0943951f) + boost[c];
            boost[c] *= 0.92f;
        }
        if (!vk.DrawFrame(rgb[0], rgb[1], rgb[2])) vk.RecreateSwapchain();

        ++frames;
        const uint64_t now = SDL_GetTicksNS();
        if (now - last_report >= 5'000'000'000ull) {
            LOGI("fps %.1f", frames * 1e9 / double(now - last_report));
            frames = 0;
            last_report = now;
        }
    }

    vk.Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    LOGI("SotC-Android M0 exit");
    return 0;
}
