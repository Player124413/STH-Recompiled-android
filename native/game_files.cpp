// The game on Android: default.xbe with its folders, or a disc image (.iso), in
//   /storage/emulated/0/Shadow          (all files access, the folder the launcher makes)
//   <app files>/assets                  (what the launcher's import copies into when it has no all files access)
//   <the top of each SD card>/Shadow
// The xbe must be the one this APK was recompiled from (its SHA-1 is baked in by CMake at build time).
#include <jni.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // android/CMakeLists.txt, from assets/default.xbe

namespace app {

namespace {

namespace fs = std::filesystem;

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return !out.empty();
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// the folders the launcher made on each storage volume, from Java: phones with several SD cards do not all show them
// under /storage
std::vector<fs::path> CardFolders() {
    std::vector<fs::path> out;
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return out;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, "cardFolderPaths", "()[Ljava/lang/String;");
    auto paths = m ? static_cast<jobjectArray>(env->CallObjectMethod(activity, m)) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        paths = nullptr;
    }
    for (jsize i = 0, n = paths ? env->GetArrayLength(paths) : 0; i < n; i++) {
        auto s = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
        if (!s) continue;
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (c) out.emplace_back(c);
        if (c) env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
    }
    return out;
}

std::vector<fs::path> GameFolders() {
    std::vector<fs::path> dirs;
    auto add = [&](fs::path p) {
        if (p.empty()) return;
        for (const fs::path& d : {p, p / "assets"}) {
            std::error_code ec;
            if (fs::is_directory(d, ec) &&
                std::find(dirs.begin(), dirs.end(), d) == dirs.end())
                dirs.push_back(d);
        }
    };
    add(ExeDir());
    add("/storage/emulated/0/Shadow");
    for (const fs::path& card : CardFolders()) add(card);
    // last: the app's own folder on each volume, where the importer put the image when there is no all files access
    if (auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv())) {
        if (auto* activity = static_cast<jobject>(SDL_GetAndroidActivity())) {
            jclass cls = env->GetObjectClass(activity);
            jmethodID m = env->GetMethodID(cls, "ownFolderPaths", "()[Ljava/lang/String;");
            auto paths = m ? static_cast<jobjectArray>(env->CallObjectMethod(activity, m)) : nullptr;
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                paths = nullptr;
            }
            for (jsize i = 0, n = paths ? env->GetArrayLength(paths) : 0; i < n; i++) {
                auto s = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
                if (!s) continue;
                const char* c = env->GetStringUTFChars(s, nullptr);
                if (c) add(c);
                if (c) env->ReleaseStringUTFChars(s, c);
                env->DeleteLocalRef(s);
            }
        }
    }
    return dirs;
}

}  // namespace

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool FindGame(GameFiles& game) {
    std::vector<fs::path> dirs = GameFolders();
    LOGI("game: looking in %zu folders, first %s", dirs.size(), dirs.empty() ? "-" : dirs[0].string().c_str());

    // a folder with the game's own files wins over any image: no disc read at all
    std::vector<std::pair<fs::path, std::vector<uint8_t>>> xbes;
    std::vector<fs::path> isos;
    for (const fs::path& dir : dirs) {
        std::error_code ec;
        std::vector<uint8_t> xbe;
        if (ReadHostFile(dir / IMAGE_XBE, xbe)) xbes.push_back({dir, std::move(xbe)});
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            if (Lower(e.path().extension().string()) == ".iso") isos.push_back(e.path());
        }
    }
    std::sort(isos.begin(), isos.end());

    std::string other;
    for (auto& [dir, xbe] : xbes) {
        std::string sha = Sha1(xbe);
        LOGI("game: %s in %s, sha1 %s", IMAGE_XBE, dir.string().c_str(), sha.c_str());
        if (sha == kImageSha1) {
            game = {dir, std::move(xbe)};
            LOGI("game: files, %s", dir.string().c_str());
            return true;
        }
        if (other.empty()) other = dir.string();
    }
    for (const fs::path& iso : isos) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string error;
        if (!disc->Open(iso, error)) {
            LOGW("game: %s: %s", iso.string().c_str(), error.c_str());
            continue;
        }
        std::vector<uint8_t> xbe;
        if (!disc->Load(IMAGE_XBE, xbe)) {
            LOGW("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
            continue;
        }
        std::string sha = Sha1(xbe);
        LOGI("game: %s from %s, sha1 %s", IMAGE_XBE, iso.string().c_str(), sha.c_str());
        if (sha != kImageSha1) {
            if (other.empty()) other = iso.string();
            continue;
        }
        game = {iso.parent_path(), std::move(xbe)};
        kernel::MountDisc(std::move(disc));
        LOGI("game: disc image, %s", iso.string().c_str());
        return true;
    }

    std::string text;
    if (other.empty())
        text = "Shadow the Hedgehog was not found.\n\nPut its disc image (.iso) in the Shadow folder on your phone "
               "(/storage/emulated/0/Shadow), or import it from the launcher.";
    else
        text = "The game in " + other +
               " is a different release than this build was made from (the USA disc).\n\nUse the USA disc image.";
    // the activity explains where the game goes; this only says why the run has nothing to start
    LOGW("game: %s", text.c_str());
    return false;
}

}  // namespace app
