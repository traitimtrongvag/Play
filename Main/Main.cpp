#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <cstdio>
#include <ctime>

#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"
#include "Menu/Jni.hpp"
#include "Includes/Macros.h"

#define targetLibName OBFUSCATE("libil2cpp.so")

typedef void (*FuncVoid)(void*);

// ==== Global state ====
static void* g_player = nullptr;
static bool autoFishingEnabled = false;
static bool hasCastRod = false;
static bool isWaitingForBite = false;
static bool bitePending = false;
static uint64_t biteTimeMs = 0;
static bool castRequest = false;
static uint64_t lastCastTimeMs = 0;
static pthread_mutex_t fishingLock = PTHREAD_MUTEX_INITIALIZER;
static bool lockCamEnabled = false;

// ==== Original hooks ====
FuncVoid orig_OnClickFishing     = nullptr;
FuncVoid orig_FishingBite        = nullptr;
FuncVoid orig_FishingCancel      = nullptr;
FuncVoid orig_UpdateFishing      = nullptr;
FuncVoid orig_OnCatchAniComplete = nullptr;

// ==== Time ====
uint64_t currentTimeMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

// ==== Log ====
void logToFile(const char* fmt, ...) {
    FILE* fp = fopen("/storage/emulated/0/Android/data/com.vng.playtogether/files/loggame.txt", "a+");
    if (!fp) return;
    va_list args;
    va_start(args, fmt);
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "[%Y-%m-%d %H:%M:%S]", tm_info);
    fprintf(fp, "%s ", timeBuf);
    vfprintf(fp, fmt, args);
    fprintf(fp, "\n");
    va_end(args);
    fclose(fp);
}

// ==== Reset fishing state ====
void resetFishingState() {
    pthread_mutex_lock(&fishingLock);
    hasCastRod = false;
    isWaitingForBite = false;
    bitePending = false;
    castRequest = false;
    pthread_mutex_unlock(&fishingLock);
    logToFile("[Auto] Fishing state reset");
}

// ==== Hooks ====
void my_UpdateFishing(void* instance) {
    static uint64_t lastUpdateMs = 0;
    uint64_t now = currentTimeMs();
    if (!instance) return;

    pthread_mutex_lock(&fishingLock);

    if (now - lastUpdateMs >= 1000) {
        g_player = instance;
        // Nếu bật khóa cam, gán _isBigFishingCam = true
if (lockCamEnabled && g_player) {
    *((bool*)((uintptr_t)g_player + 0x557)) = true;
} else if (!lockCamEnabled && g_player) {
    *((bool*)((uintptr_t)g_player + 0x557)) = false;
}
        uintptr_t* vtable = *(uintptr_t**)instance;
        logToFile("UpdateFishing: this=%p, vtable[0]=0x%lx",
                  instance, vtable ? vtable[0] : 0UL);
        lastUpdateMs = now;
    }

    // 1. Gọi OnClickFishing lần đầu
    if (autoFishingEnabled && castRequest && !hasCastRod && g_player) {
        if (now - lastCastTimeMs > 1500) {
            logToFile("[Auto] Cast rod");
            hasCastRod = true;
            isWaitingForBite = true;
            castRequest = false;
            lastCastTimeMs = now;
            if (orig_OnClickFishing)
                orig_OnClickFishing(g_player);
        }
    }

    // 2. Gọi OnClickFishing lần 2 khi cá cắn
    if (autoFishingEnabled && bitePending && g_player) {
        if (now - biteTimeMs < 500) {
            logToFile("[Auto] Bite detected → Reel in");
            if (orig_OnClickFishing)
                orig_OnClickFishing(g_player);
        }
    }

    pthread_mutex_unlock(&fishingLock);

    if (orig_UpdateFishing)
        orig_UpdateFishing(instance);
}

void my_FishingBite(void* instance) {
    pthread_mutex_lock(&fishingLock);
    bool doReel = autoFishingEnabled && hasCastRod && isWaitingForBite && g_player;
    isWaitingForBite = false;
    hasCastRod = false;
    if (doReel) {
        logToFile("[Auto] Fish bite → start bite sequence");
        bitePending = true;
        biteTimeMs = currentTimeMs();
    }
    pthread_mutex_unlock(&fishingLock);

    if (orig_FishingBite)
        orig_FishingBite(instance);
}

void my_OnCatchAniComplete(void* instance) {
    logToFile("[Hook] OnCatchAniComplete");

    pthread_mutex_lock(&fishingLock);
    bitePending = false;
    pthread_mutex_unlock(&fishingLock);

    if (orig_FishingCancel && g_player) {
        usleep(500000);  // đợi 0.5s
        logToFile("[Auto] Calling FishingCancel after ani complete");
        orig_FishingCancel(g_player);
        usleep(1000000);  // đợi 1s
    }

    resetFishingState();  // ✅ THÊM Ở ĐÂY

    if (orig_OnCatchAniComplete)
        orig_OnCatchAniComplete(instance);
}
void my_OnClickFishing(void* instance) {
    logToFile("[Hook] OnClickFishing");
    if (orig_OnClickFishing)
        orig_OnClickFishing(instance);
}

void my_FishingCancel(void* instance) {
    logToFile("[Hook] FishingCancel");
    if (orig_FishingCancel)
        orig_FishingCancel(instance);
}

// ==== Background thread ====
void* AutoFishingLoop(void*) {
    while (true) {
        if (autoFishingEnabled) {
            pthread_mutex_lock(&fishingLock);
            if (g_player && !hasCastRod && !castRequest && !bitePending) {
                castRequest = true;
                logToFile("[Auto] Requesting new cast");
            }
            pthread_mutex_unlock(&fishingLock);
        }
        usleep(200000);
    }
    return nullptr;
}

// ==== Menu ====
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
        OBFUSCATE("Toggle_Auto Câu cá"),
        OBFUSCATE("Toggle_Khóa cam"),
        OBFUSCATE("Toggle_Chế độ ngủ"),
        OBFUSCATE("Toggle_Auto Đập đá"),
        OBFUSCATE("Toggle_Auto Bắt bọ"),
    };
    int total = sizeof(features) / sizeof(features[0]);
    jobjectArray ret = env->NewObjectArray(total,
        env->FindClass("java/lang/String"),
        env->NewStringUTF(""));
    for (int i = 0; i < total; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return ret;
}

void Changes(JNIEnv*, jclass, jobject, jint featNum, jstring, jint, jlong, jboolean boolean, jstring) {
    if (featNum == 0) {
        bool prevState = autoFishingEnabled;
        autoFishingEnabled = boolean;

        if (prevState != autoFishingEnabled) {
            resetFishingState();
            logToFile("[Auto] Auto fishing %s", autoFishingEnabled ? "enabled" : "disabled");
        }
    }
    else if (featNum == 1) {
        lockCamEnabled = boolean;
        logToFile("[Auto] Lock cam %s", lockCamEnabled ? "enabled" : "disabled");
    }
}
// ==== Start thread ====
void* hack_thread(void*) {
    LOGI(OBFUSCATE("Hack thread started"));
    ElfScanner g_elf;
    do {
        sleep(1);
        g_elf = ElfScanner::createWithPath(targetLibName);
    } while (!g_elf.isValid());

    uintptr_t libBase = g_elf.base();

    HOOK(targetLibName, str2Offset("0x3a72b00"), my_UpdateFishing,      orig_UpdateFishing);
    HOOK(targetLibName, str2Offset("0x3a6cbc0"), my_OnClickFishing,     orig_OnClickFishing);
    HOOK(targetLibName, str2Offset("0x3a6c9c0"), my_FishingCancel,      orig_FishingCancel);
    HOOK(targetLibName, str2Offset("0x3a71f60"), my_FishingBite,        orig_FishingBite);
    HOOK(targetLibName, str2Offset("0x3a7b344"), my_OnCatchAniComplete, orig_OnCatchAniComplete);  // NEW

    pthread_t autoLoop;
    pthread_create(&autoLoop, nullptr, AutoFishingLoop, nullptr);
    return nullptr;
}

__attribute__((constructor))
void lib_main() {
    pthread_t t;
    pthread_create(&t, nullptr, hack_thread, nullptr);
}