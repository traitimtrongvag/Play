#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdio.h>
#include <unordered_set>
#include <sys/mman.h>

#include <thread>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>

#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"
#include "Menu/Jni.hpp"
#include "Includes/Macros.h"

#define targetLibName OBFUSCATE("libil2cpp.so")
// Mảng patch

void* tuongthis = nullptr;
bool tuongthis_found = false;
std::string user_input_message = "";
int user_input_count = 0;
bool is_running = false;
int current_sent_count = 0;
bool should_stop = false;
static pthread_mutex_t tuong_mutex = PTHREAD_MUTEX_INITIALIZER;

// ==== Function typedefs ====
typedef void (*t_SendToServerPostMessage)(void* thistuong, void* message);
t_SendToServerPostMessage old_SendToServerPostMessage;
t_SendToServerPostMessage SendToServerPostMessage;

typedef void (*t_ctor)(void* thistuong);
t_ctor old_ctor;

// ==== Helper functions ====
typedef void* (*t_il2cpp_string_new)(const char* str);
t_il2cpp_string_new il2cpp_string_new = nullptr;

void* create_mono_string(const char* text) {
    if (!text) return nullptr;

    // Nếu tìm thấy hàm gốc Unity thì dùng luôn
    if (il2cpp_string_new != nullptr) {
        return il2cpp_string_new(text);
    }

    size_t len = strlen(text);

    // struct: 16 byte (klass + monitor) + 4 byte length + 4 byte padding + UTF-16
    size_t headerSize = sizeof(void*) * 2 + sizeof(int32_t) + sizeof(int32_t); // padding
    size_t totalSize = headerSize + (len + 1) * sizeof(char16_t);

    void* unityString = malloc(totalSize);
    if (!unityString) return nullptr;

    memset(unityString, 0, totalSize);

    // Ghi length
    *((int32_t*)((char*)unityString + sizeof(void*) * 2)) = (int32_t)len;

    // Ghi UTF-16
    char16_t* chars = (char16_t*)((char*)unityString + headerSize);
    for (size_t i = 0; i < len; i++) {
        chars[i] = (char16_t)text[i];
    }
    chars[len] = 0; // null-terminator

    return unityString;
}

// ==== Hook functions ====
void SendToServerPostMessage_hook(void* thistuong, void* message) {
    old_SendToServerPostMessage(thistuong, message);
}

void ctor_hook(void* thistuong) {
    pthread_mutex_lock(&tuong_mutex);
    tuongthis = thistuong;
    tuongthis_found = true;
    pthread_mutex_unlock(&tuong_mutex);
    
    old_ctor(thistuong);
}

// ==== Sending thread ====
void* sending_thread(void* arg) {
    while (is_running && !should_stop) {
        pthread_mutex_lock(&tuong_mutex);
        void* current_tuongthis = tuongthis;
        bool has_this = tuongthis_found;
        pthread_mutex_unlock(&tuong_mutex);
        
        if (has_this && current_tuongthis != nullptr && 
            !user_input_message.empty() && 
            current_sent_count < user_input_count) {
            
            try {
                void* mono_message = create_mono_string(user_input_message.c_str());
                
                if (mono_message != nullptr && SendToServerPostMessage != nullptr) {
                    SendToServerPostMessage(current_tuongthis, mono_message);
                    current_sent_count++;
                    
                    if (current_sent_count >= user_input_count) {
                        is_running = false;
                        break;
                    }
                }
                
                if (mono_message && il2cpp_string_new == nullptr) {
                    free(mono_message);
                }
                
            } catch (...) {
            }
        }
        
        usleep(500000);
    }
    
    return nullptr;
}
pthread_mutex_t mutex_nop = PTHREAD_MUTEX_INITIALIZER;
bool nop_patched = false;
uint8_t backup_bytes[8] = {0};

void patchNOP(uintptr_t addr) {
    pthread_mutex_lock(&mutex_nop);

    if (addr != 0 && !nop_patched) {
        // Sao lưu 8 bytes gốc để có thể restore nếu cần
        memcpy(backup_bytes, (void*)addr, 8);

        uint32_t ret_instruction = 0xD65F03C0; // lệnh ARM64 ret (4 bytes)
        // Ghi thêm 4 bytes NOP nếu muốn full 8 bytes thì có thể bổ sung

        if (mprotect((void*)(addr & ~0xFFF), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            // Ghi 4 bytes ret đầu tiên
            memcpy((void*)addr, &ret_instruction, sizeof(ret_instruction));

            // Ghi 4 bytes NOP (0x1F, 0x20, 0x03, 0xD5) hoặc fill 0 nếu muốn
            uint32_t nop_4bytes = 0xD503201F; // NOP ARM64
            memcpy((void*)(addr + 4), &nop_4bytes, sizeof(nop_4bytes));

            nop_patched = true;

            mprotect((void*)(addr & ~0xFFF), 0x1000, PROT_READ | PROT_EXEC);

            printf("[PATCH] NOP patched (8 bytes) at %p\n", (void*)addr);
        } else {
            printf("[PATCH] Failed to change memory protection for NOP patch\n");
        }
    }

    pthread_mutex_unlock(&mutex_nop);
}
pthread_mutex_t mutex_ret_false = PTHREAD_MUTEX_INITIALIZER;
bool ret_false_patched = false;  // false để patch lần đầu được

uint8_t patch_return_false[] = {
    0x00, 0x00, 0x80, 0x52,  // mov w0, #0
    0xC0, 0x03, 0x5F, 0xD6   // ret
};

void patchReturnFalse(uintptr_t addr) {
    pthread_mutex_lock(&mutex_ret_false);

    if (addr != 0 && !ret_false_patched) {
        if (mprotect((void*)(addr & ~0xFFF), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            memcpy((void*)addr, patch_return_false, sizeof(patch_return_false));
            ret_false_patched = true;
            mprotect((void*)(addr & ~0xFFF), 0x1000, PROT_READ | PROT_EXEC);
            printf("[PATCH] Return false patched at %p\n", (void*)addr);
        } else {
            printf("[PATCH] Failed to change memory protection for return false patch\n");
        }
    }

    pthread_mutex_unlock(&mutex_ret_false);
}
struct Vector3 {
    float x, y, z;
};

struct Quaternion {
    float x, y, z, w;
};

float saved_x = 0.0f, saved_y = 0.0f, saved_z = 0.0f;
void *mainPlayer = nullptr;
bool mainPlayerFound = false;

Vector3 (*get_TransientPosition)(void *thisplayer);
void (*SetPosition)(void *thisplayer, Vector3 position, bool bypassInterpolation);
void (*SetPositionAndRotation)(void *thisplayer, Vector3 position, Quaternion rotation, bool bypassInterpolation);
bool (*get_Boolean)(void *thisplayer);

bool (*old_get_Boolean_hook)(void *thisplayer);
typedef bool (*t_get_Boolean)(void *thisplayer);
t_get_Boolean old_get_Boolean;
std::unordered_set<void*> processed_bugs;  
// Optimized bug collection variables
// Track bugs we already called Init_Bug on
bool gom_bo_enabled = false;
bool teleport_called = false;  // Flag to track if teleport was already called
static pthread_mutex_t bug_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool onMiss_patched = false;
// Global biến địa chỉ hàm OnMiss
void* onMissAddr = nullptr;
// Function pointers for bug collection
void (*Init_Bug)(void *thisbo, Vector3 spawnPos, uint32_t zoneID, bool isTestMode);

typedef void (*t_LateUpdate_Bug)(void *thisbo);
t_LateUpdate_Bug old_LateUpdate_Bug;
typedef Vector3 (*t_FindSamplePosition)(void *thisbo, Vector3 pos);
t_FindSamplePosition old_FindSamplePosition;

// --- New teleport input variables ---
float input_pos_x = 0.0f, input_pos_y = 0.0f, input_pos_z = 0.0f;


struct DelayedBug {
    void* bug_ptr;
    std::chrono::steady_clock::time_point execute_time;
};

std::queue<DelayedBug> delayed_bugs;
std::mutex delayed_bugs_mutex;
std::thread* delay_worker_thread = nullptr;
std::condition_variable delay_cv;
bool delay_worker_running = false;
void Teleportplayer() {
    if (gom_bo_enabled && mainPlayer && SetPositionAndRotation) {
        Vector3 teleport_pos = {-182.47845f, 4.8944f, -55.71112f};
        Quaternion teleport_rot = {0.0f, 0.44654f, 0.0f, 0.89476f};
        SetPositionAndRotation(mainPlayer, teleport_pos, teleport_rot, true);
    }
}


// Hook for bug LateUpdate - only call Init_Bug once per new bug instance
void LateUpdate_Bug_hook(void* thisbo) {
Vector3 teleport_pos = {-182.47845458984375f, 4.8944091796875f, -55.71112060546875f};
Quaternion teleport_rot = {-0.0f, 0.44654539227485657f, -0.0f, 0.8947610259056091f};

if (gom_bo_enabled && !onMiss_patched) {

uint8_t patch_onMiss[] = {  
    0x00, 0x00, 0x80, 0x52,  
    0xC0, 0x03, 0x5F, 0xD6  
};  
mprotect((void *)((uintptr_t)onMissAddr & ~0xFFF), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);  
memcpy((void*)onMissAddr, patch_onMiss, sizeof(patch_onMiss));  
onMiss_patched = true;

}

if (gom_bo_enabled && thisbo != nullptr && Init_Bug != nullptr) {
// Teleport player to collection position only once when feature is first enabled

     
    pthread_mutex_lock(&bug_mutex);  

    if (processed_bugs.find(thisbo) == processed_bugs.end()) {      
        processed_bugs.insert(thisbo);      
              
        pthread_mutex_unlock(&bug_mutex);      
              
        
              
        // Use the new bug collection position instead of 0,0,0  
        Vector3 spawn_pos = {-181.881591796875f, 4.8944091796875f, -54.929443359375f};        
        uint32_t zone_id = 0;        
        bool is_test_mode = false;        
              
        try {        
            Init_Bug(thisbo, spawn_pos, zone_id, is_test_mode);        
        } catch (...) {        
        }      
          
    } else {      
        pthread_mutex_unlock(&bug_mutex);      
    }      
          
    static int cleanup_counter = 0;      
    if (++cleanup_counter >= 500) {      
        cleanup_counter = 0;      
        pthread_mutex_lock(&bug_mutex);      
        if (processed_bugs.size() > 1000) {      
            processed_bugs.clear();      
        }      
        pthread_mutex_unlock(&bug_mutex);      
    }      
}        
    
old_LateUpdate_Bug(thisbo);

}




Vector3 FindSamplePosition_hook(void* thisbo, Vector3 pos) {
    if (gom_bo_enabled && thisbo != nullptr) {
        // Return the new bug collection position instead of 0,0,0
        Vector3 collection_vector = {-181.881591796875f, 4.8944091796875f, -54.929443359375f};
        return collection_vector;
    }
    return old_FindSamplePosition(thisbo, pos);
}

bool get_Boolean_hook(void* thisplayer) {
    bool result = old_get_Boolean(thisplayer);

    if (thisplayer != nullptr && !mainPlayerFound) {      
        if (result) {      
            mainPlayer = thisplayer;      
            mainPlayerFound = true;      
        }      
    }      
      
    return result;
}

enum FishingState {
    None = 0, Casting = 1, Search = 2, SearchResult = 3, Idle = 4,
    Hit = 5, Fighting = 6, Catch = 7, Fail = 8, Boast = 9,
    Finish = 10, CastingFail = 11, Miss = 12,BigFish_RaidEnter = 13
};

enum FishingFailType {
    FishingFailType_None = 0
};

typedef bool (StartFishingFunc)(void*);
typedef bool (CheckRodNeedsRepairFunc)(void*);
typedef void (RepairRodFunc)(void*);
typedef void (OnFishingStartFunc)(void*, uint32_t);
typedef bool (UpdateFishingStateFunc)(void*, int);
typedef void (ReceiveFishingBeginFunc)(void*, bool, uint32_t, bool, int);
typedef void (ShowRepairItemFunc)(void*, int);

StartFishingFunc* bat_dau_cau_ca = nullptr;
CheckRodNeedsRepairFunc* kiem_tra_can_sua = nullptr;
RepairRodFunc* sua_can_cau = nullptr;
OnFishingStartFunc* khi_bat_dau_cau = nullptr;
UpdateFishingStateFunc* cap_nhat_trang_thai_cau = nullptr;
ReceiveFishingBeginFunc* nhan_bat_dau_cau = nullptr;
ShowRepairItemFunc* show_repair_item = nullptr;

static void* this_cua_player = nullptr;
static void* this_sua_can = nullptr;
static bool bat_auto_cc = false;
static bool bat_auto_cc_nhanh = false;
static bool bat_khoa_cam = false;
static bool bat_fake_bong = false;
static bool bat_fake_bong1 = false;
static bool Goi_bong = false;
static bool da_quang_can = false;
static bool doi_ca_can = false;
static bool ca_can = false;
static bool co_the_quangcan = true;
static bool nen_cancel = false;
static bool sua_xong = false;
static bool da_keo_ca = false;
static bool bat_giu_bao_quan = false;
static bool cho_reset = false;
static bool repair_dialog_called = false;
static bool need_repair_call = false;
static bool pause_startfishing = false;
static uint64_t thoi_gian_can_ms = 0;
static uint64_t thoi_gian_quang_cuoi_ms = 0;
static uint64_t thoi_gian_bat_dau_cancel_ms = 0;

static bool dang_cho_bat_dau_cau = false;
static bool bat_dau_cau_cho_xu_ly = false;
static bool bat_dau_cau_da_xu_ly = false;
static uint64_t thoi_gian_bat_dau_cau_ms = 0;

static bool can_bat_ca = false;
static bool bat_ca_da_xu_ly = false;
static uint64_t thoi_gian_bat_ca_ms = 0;

static uintptr_t dia_chi_ham_camera = 0;
static bool ham_camera_da_vo_hieu = false;
static uint8_t byte_goc[8] = {0};

static pthread_mutex_t khoa_cau_ca = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t khoa_camera = PTHREAD_MUTEX_INITIALIZER;

typedef void (FuncVoid)(void*);
typedef void (FuncVoidUint32)(void*, uint32_t);
typedef bool (FuncBoolInt)(void*, int);
typedef void (FuncVoidInt)(void*, int);
typedef void (*FuncVoidPtrInt64)(void*, int64_t);
FuncVoidPtrInt64 goc_OnClickLift = nullptr;
typedef void FuncVoid(void*);

typedef void FuncVoid2(void*, int);

FuncVoid2* goc_OnClickFishing = nullptr;




FuncVoid* goc_FishingBite = nullptr;
FuncVoid* goc_FishingCancel = nullptr;
FuncVoid* goc_UpdateFishing = nullptr;
FuncVoid* goc_OnCatchAniComplete = nullptr;
FuncVoid* goc_FishBiteOrDash = nullptr;
FuncVoid* goc_LateUpdate = nullptr;
FuncVoidUint32* goc_OnFishingStart = nullptr;
FuncBoolInt* goc_UpdateFishingState = nullptr;
FuncVoidInt* goc_ShowRepairItem = nullptr;

uint64_t lay_thoi_gian_hien_tai_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

void bat_khoa_camera() {
    pthread_mutex_lock(&khoa_camera);
    
    if (dia_chi_ham_camera && !ham_camera_da_vo_hieu) {
        memcpy(byte_goc, (void*)dia_chi_ham_camera, 8);
        uint32_t lenh_return = 0xD65F03C0;
        
        if (mprotect((void*)(dia_chi_ham_camera & ~0xFFF), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            memcpy((void*)dia_chi_ham_camera, &lenh_return, 4);
            ham_camera_da_vo_hieu = true;
            mprotect((void*)(dia_chi_ham_camera & ~0xFFF), 0x1000, PROT_READ | PROT_EXEC);
        }
    }
    
    pthread_mutex_unlock(&khoa_camera);
}

void tat_khoa_camera() {
    pthread_mutex_lock(&khoa_camera);
    
    if (dia_chi_ham_camera && ham_camera_da_vo_hieu) {
        if (mprotect((void*)(dia_chi_ham_camera & ~0xFFF), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            memcpy((void*)dia_chi_ham_camera, byte_goc, 8);
            ham_camera_da_vo_hieu = false;
            mprotect((void*)(dia_chi_ham_camera & ~0xFFF), 0x1000, PROT_READ | PROT_EXEC);
        }
    }
    
    pthread_mutex_unlock(&khoa_camera);
}

void reset_trang_thai_cau_ca() {
    pthread_mutex_lock(&khoa_cau_ca);
    da_quang_can = false;
    doi_ca_can = false;
    ca_can = false;
    da_keo_ca = false;
    dang_cho_bat_dau_cau = false;
    bat_dau_cau_cho_xu_ly = false;
    bat_dau_cau_da_xu_ly = false;
    can_bat_ca = false;
    bat_ca_da_xu_ly = false;
    Goi_bong = false;
    repair_dialog_called = false;
    need_repair_call = false;
    pause_startfishing = false;
    pthread_mutex_unlock(&khoa_cau_ca);
}

void toi_ShowRepairItem(void* instance, int command) {
    if (goc_ShowRepairItem) goc_ShowRepairItem(instance, command);
    
    pthread_mutex_lock(&khoa_cau_ca);
    if ((bat_auto_cc || bat_auto_cc_nhanh) && instance == this_sua_can) {
        repair_dialog_called = true;
        need_repair_call = true;
        pause_startfishing = true;
    }
    pthread_mutex_unlock(&khoa_cau_ca);
}

void toi_LateUpdate(void* instance) {
    if (instance) {
        pthread_mutex_lock(&khoa_cau_ca);
        this_sua_can = instance;
        pthread_mutex_unlock(&khoa_cau_ca);
    }
    if (goc_LateUpdate) goc_LateUpdate(instance);
}

void toi_OnFishingStart(void* instance, uint32_t muc_do_kho) {
    pthread_mutex_lock(&khoa_cau_ca);
    
    if (bat_auto_cc_nhanh && dang_cho_bat_dau_cau) {
        bat_dau_cau_cho_xu_ly = true;
        bat_dau_cau_da_xu_ly = false;
        thoi_gian_bat_dau_cau_ms = lay_thoi_gian_hien_tai_ms();
        dang_cho_bat_dau_cau = false;
    }
    
    pthread_mutex_unlock(&khoa_cau_ca);
    
    if (goc_OnFishingStart) 
        goc_OnFishingStart(instance, muc_do_kho);
}

bool toi_UpdateFishingState(void* instance, int trang_thai_cau_ca) {
    if (goc_UpdateFishingState)
        return goc_UpdateFishingState(instance, trang_thai_cau_ca);
    return false;
}

void toi_UpdateFishing(void* instance) {
    uint64_t hien_tai = lay_thoi_gian_hien_tai_ms();
    if (!instance) return;

    pthread_mutex_lock(&khoa_cau_ca);
    this_cua_player = instance;

    if (bat_fake_bong && !bat_fake_bong1 && this_cua_player && nhan_bat_dau_cau) {
        nhan_bat_dau_cau(this_cua_player, true, 42, true, FishingFailType_None);
        bat_fake_bong1 = true;
    }
    if (Goi_bong && this_cua_player && cap_nhat_trang_thai_cau) {
        bool ket_qua = cap_nhat_trang_thai_cau(this_cua_player, BigFish_RaidEnter);
        Goi_bong = false;
    }

    if (cho_reset && this_cua_player && goc_FishingCancel) {
        goc_FishingCancel(this_cua_player);
        cho_reset = false;
    }

    if (need_repair_call && repair_dialog_called && this_sua_can && sua_can_cau) {
        sua_can_cau(this_sua_can);
        need_repair_call = false;
        repair_dialog_called = false;
        pause_startfishing = false;
    }

    if ((bat_auto_cc || bat_auto_cc_nhanh) && this_cua_player && !pause_startfishing) {
        if (hien_tai - thoi_gian_quang_cuoi_ms >= 200) {
            bat_dau_cau_ca(this_cua_player);
            thoi_gian_quang_cuoi_ms = hien_tai;
            
            if (bat_auto_cc) {
                da_quang_can = true;
                doi_ca_can = true;
            } else if (bat_auto_cc_nhanh) {
                dang_cho_bat_dau_cau = true;
            }
        }
    }

    if (bat_auto_cc_nhanh) {
        if (bat_dau_cau_cho_xu_ly && !bat_dau_cau_da_xu_ly && hien_tai - thoi_gian_bat_dau_cau_ms < 500) {
            bool ket_qua = cap_nhat_trang_thai_cau(this_cua_player, Fighting);
            bat_dau_cau_da_xu_ly = true;
            can_bat_ca = true;
            bat_ca_da_xu_ly = false;
            thoi_gian_bat_ca_ms = hien_tai + 400;
        }

        if (can_bat_ca && !bat_ca_da_xu_ly && hien_tai >= thoi_gian_bat_ca_ms) {
            bool ket_qua = cap_nhat_trang_thai_cau(this_cua_player, Catch);
            bat_ca_da_xu_ly = true;
            can_bat_ca = false;
        }
    }

    if (bat_auto_cc && !bat_auto_cc_nhanh && ca_can && !da_keo_ca && hien_tai - thoi_gian_can_ms < 500) {
        goc_OnClickFishing(this_cua_player,0);
        da_keo_ca = true;
    }

    if (nen_cancel) {
        uint64_t thoi_gian_tre = bat_giu_bao_quan ? 400 : 0;
        if (hien_tai - thoi_gian_bat_dau_cancel_ms > thoi_gian_tre) {
            goc_FishingCancel(this_cua_player);
            nen_cancel = false;
            co_the_quangcan = true;
        }
    }

    pthread_mutex_unlock(&khoa_cau_ca);
    if (goc_UpdateFishing) goc_UpdateFishing(instance);
}

void toi_FishingBite(void* instance) {
    pthread_mutex_lock(&khoa_cau_ca);
    
    if (!bat_auto_cc_nhanh && bat_auto_cc && da_quang_can && doi_ca_can) {
        doi_ca_can = false;
        da_quang_can = false;
        ca_can = true;
        da_keo_ca = false;
        thoi_gian_can_ms = lay_thoi_gian_hien_tai_ms();
    }
    
    pthread_mutex_unlock(&khoa_cau_ca);
    if (goc_FishingBite) goc_FishingBite(instance);
}

void toi_FishBiteOrDash(void* instance) {
    pthread_mutex_lock(&khoa_cau_ca);
    
    if (!bat_auto_cc_nhanh && bat_auto_cc && goc_OnClickFishing) {
        goc_OnClickFishing(this_cua_player,0);
    }
    
    pthread_mutex_unlock(&khoa_cau_ca);
    if (goc_FishBiteOrDash) goc_FishBiteOrDash(instance);
}

void toi_OnCatchAniComplete(void* instance) {
    pthread_mutex_lock(&khoa_cau_ca);
    
    ca_can = false;
    da_keo_ca = false;
    if (bat_auto_cc || bat_auto_cc_nhanh) {
        nen_cancel = true;
        thoi_gian_bat_dau_cancel_ms = lay_thoi_gian_hien_tai_ms();
    }
    
    pthread_mutex_unlock(&khoa_cau_ca);
    reset_trang_thai_cau_ca();
    if (goc_OnCatchAniComplete) goc_OnCatchAniComplete(instance);
}

void toi_OnClickFishing(void* instance) {
    pthread_mutex_lock(&khoa_cau_ca);
    co_the_quangcan = false;
    pthread_mutex_unlock(&khoa_cau_ca);
    if (goc_OnClickFishing) goc_OnClickFishing(instance,0);
}

void toi_FishingCancel(void* instance) {
    if (goc_FishingCancel) goc_FishingCancel(instance);
}

jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
    OBFUSCATE("Collapse_Câu cá"),

OBFUSCATE("0_CollapseAdd_Toggle_Auto Câu cá"),
OBFUSCATE("1_CollapseAdd_Toggle_Auto câu cá nhanh"),
OBFUSCATE("2_CollapseAdd_Toggle_Khóa cam"),
OBFUSCATE("3_CollapseAdd_Toggle_Fake bóng"),
OBFUSCATE("4_CollapseAdd_Button_Gọi b7"),
OBFUSCATE("5_CollapseAdd_Button_Reset(nêu lag)"),
OBFUSCATE("6_CollapseAdd_Toggle_Giữ bảo quản"),
OBFUSCATE("Collapse_Côn Trùng"),
 OBFUSCATE("7_CollapseAdd_Toggle_Auto bắt bọ"),
OBFUSCATE("Collapse_Tiện ích"),
OBFUSCATE("8_CollapseAdd_Button_Dịch chuyển"),
 OBFUSCATE("9_CollapseAdd_Button_Lưu vị trí"),
 OBFUSCATE("10_CollapseAdd_InputValue_Pos X"),
 OBFUSCATE("11_CollapseAdd_InputValue_Pos Y"),
 OBFUSCATE("12_CollapseAdd_InputValue_Pos Z"),
 OBFUSCATE("13_CollapseAdd_Button_Teleport"),
 OBFUSCATE("14_CollapseAdd_InputValue_Nhap so luong"),
        OBFUSCATE("15_CollapseAdd_InputText_Nhap text"),
        OBFUSCATE("16_CollapseAdd_Toggle_Chay")
 
    };
    int total = sizeof(features) / sizeof(features[0]);
    jobjectArray ret = env->NewObjectArray(total, env->FindClass("java/lang/String"), env->NewStringUTF(""));
    for (int i = 0; i < total; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return ret;
}

void Changes(JNIEnv* env, jclass, jobject, jint featNum, jstring featName, jint value, jlong, jboolean boolean, jstring str) {
    switch (featNum) {
        case 0:
            bat_auto_cc = boolean;
            if (boolean && bat_auto_cc_nhanh) {
                bat_auto_cc_nhanh = false;
            }
            reset_trang_thai_cau_ca();
            pthread_mutex_lock(&khoa_cau_ca);
            co_the_quangcan = true;
            nen_cancel = false;
            sua_xong = false;
            da_keo_ca = false;
            pthread_mutex_unlock(&khoa_cau_ca);
            break;

        case 1:
            bat_auto_cc_nhanh = boolean;
            if (boolean && bat_auto_cc) {
                bat_auto_cc = false;
            }
            reset_trang_thai_cau_ca();
            pthread_mutex_lock(&khoa_cau_ca);
            co_the_quangcan = true;
            nen_cancel = false;
            sua_xong = false;
            da_keo_ca = false;
            bat_dau_cau_cho_xu_ly = false;
            bat_dau_cau_da_xu_ly = false;
            dang_cho_bat_dau_cau = false;
            can_bat_ca = false;
            bat_ca_da_xu_ly = false;
            pthread_mutex_unlock(&khoa_cau_ca);
            break;

        case 2:
            bat_khoa_cam = boolean;
            if (boolean) {
                bat_khoa_camera();
            } else {
                tat_khoa_camera();
            }
            break;

        case 3:
            bat_fake_bong = boolean;
            pthread_mutex_lock(&khoa_cau_ca);
            bat_fake_bong1 = false;
            pthread_mutex_unlock(&khoa_cau_ca);
            break;

        case 4:
            if (this_cua_player) {
                pthread_mutex_lock(&khoa_cau_ca);
                Goi_bong = true;
                pthread_mutex_unlock(&khoa_cau_ca);
            }
            break;

        case 5:
            if (this_cua_player) {
                pthread_mutex_lock(&khoa_cau_ca);
                cho_reset = true;
                pthread_mutex_unlock(&khoa_cau_ca);
            }
            break;

        case 6:
            bat_giu_bao_quan = boolean;
            break;

        case 7:
            gom_bo_enabled = boolean;
            pthread_mutex_lock(&bug_mutex);
            
            if (!boolean) {
                processed_bugs.clear();
                teleport_called = false;  // Reset teleport flag when disabled
            }
            pthread_mutex_unlock(&bug_mutex);
            break;

        case 8:
            if (mainPlayer != nullptr && SetPosition != nullptr && mainPlayerFound) {
                Vector3 targetPos = {saved_x, saved_y, saved_z};
                SetPosition(mainPlayer, targetPos, true);
                usleep(50000);
            }
            break;

        case 9:
            if (mainPlayer != nullptr && get_TransientPosition != nullptr && mainPlayerFound) {
                Vector3 currentPosition = get_TransientPosition(mainPlayer);
                if (currentPosition.x == 0.0f && currentPosition.y == 0.0f && currentPosition.z == 10000.0f) {
                    usleep(100000);
                    currentPosition = get_TransientPosition(mainPlayer);
                }
                saved_x = currentPosition.x;
                saved_y = currentPosition.y;
                saved_z = currentPosition.z;
            }
            break;

        case 10:
            input_pos_x = (float)value;
            break;

        case 11:
            input_pos_y = (float)value;
            break;

        case 12:
            input_pos_z = (float)value;
            break;

        case 13:
            if (mainPlayer != nullptr && SetPosition != nullptr && mainPlayerFound) {
                Vector3 targetPos = {input_pos_x, input_pos_y, input_pos_z};
                SetPosition(mainPlayer, targetPos, true);
                usleep(50000);
            }
            break;
       case 14: // Nhập số lượng
            user_input_count = value;
            break;

        case 15: // Nhập text
            if (str != nullptr) {
                const char* text = env->GetStringUTFChars(str, 0);
                user_input_message = std::string(text);
                env->ReleaseStringUTFChars(str, text);
            }
            break;

        case 16: // Toggle Chay
            if (boolean) {
                if (!user_input_message.empty() && user_input_count > 0 && tuongthis_found) {
                    is_running = true;
                    should_stop = false;
                    current_sent_count = 0;
                    
                    pthread_t sending_t;
                    pthread_create(&sending_t, nullptr, sending_thread, nullptr);
                    pthread_detach(sending_t);
                }
            } else {
                is_running = false;
                should_stop = true;
                current_sent_count = 0;
            }
            break;
    
  
    }
}

void* hack_thread(void*) {
    ElfScanner g_elf;

    do {
        usleep(4000000);
        g_elf = ElfScanner::createWithPath(targetLibName);
    } while (!g_elf.isValid());
    uintptr_t libBase = g_elf.base();

    bat_dau_cau_ca = (StartFishingFunc*)(libBase + str2Offset("0x3c1e67c"));
    kiem_tra_can_sua = (CheckRodNeedsRepairFunc*)(libBase + str2Offset("0x3c29494"));
    sua_can_cau = (RepairRodFunc*)(libBase + str2Offset("0x351c4b8"));
    khi_bat_dau_cau = (OnFishingStartFunc*)(libBase + str2Offset("0x3c22d44"));
    cap_nhat_trang_thai_cau = (UpdateFishingStateFunc*)(libBase + str2Offset("0x3c1f7c0"));
    nhan_bat_dau_cau = (ReceiveFishingBeginFunc*)(libBase + str2Offset("0x3c22680"));
    show_repair_item = (ShowRepairItemFunc*)(libBase + str2Offset("0x351c4a0"));
    dia_chi_ham_camera = libBase + str2Offset("0x455de68");
    onMissAddr = (void *)(libBase + str2Offset("0x3502b48"));

    HOOK(targetLibName, str2Offset("0x3c23f44"), toi_UpdateFishing, goc_UpdateFishing);
    HOOK(targetLibName, str2Offset("0x3c1f7b0"), toi_OnClickFishing, goc_OnClickFishing);
    
    HOOK(targetLibName, str2Offset("0x3c1de34"), toi_FishingCancel, goc_FishingCancel);
    HOOK(targetLibName, str2Offset("0x3c233a4"), toi_FishingBite, goc_FishingBite);
    HOOK(targetLibName, str2Offset("0x3c2c7b8"), toi_OnCatchAniComplete, goc_OnCatchAniComplete);
    HOOK(targetLibName, str2Offset("0x3c24e34"), toi_FishBiteOrDash, goc_FishBiteOrDash);
    HOOK(targetLibName, str2Offset("0x351597c"), toi_LateUpdate, goc_LateUpdate);
    HOOK(targetLibName, str2Offset("0x3c22d44"), toi_OnFishingStart, goc_OnFishingStart);
    HOOK(targetLibName, str2Offset("0x3c1f7c0"), toi_UpdateFishingState, goc_UpdateFishingState);
    HOOK(targetLibName, str2Offset("0x351c4a0"), toi_ShowRepairItem, goc_ShowRepairItem);
get_TransientPosition = (Vector3 (*)(void *)) (libBase + str2Offset("0x386E93C"));      
    SetPosition = (void (*)(void *, Vector3, bool)) (libBase + str2Offset("0x386F668"));      
    SetPositionAndRotation = (void (*)(void *, Vector3, Quaternion, bool)) (libBase + str2Offset("0x386f7ec"));
    get_Boolean = (bool (*)(void *)) (libBase + str2Offset("0x386e980"));      
          
    Init_Bug = (void (*)(void *, Vector3, uint32_t, bool)) (libBase + str2Offset("0x3500c3c"));      
      
    HOOK(targetLibName, str2Offset("0x386e980"), get_Boolean_hook, old_get_Boolean);      
    HOOK(targetLibName, str2Offset("0x3502cf4"), LateUpdate_Bug_hook, old_LateUpdate_Bug);      
    HOOK(targetLibName, str2Offset("0x3501498"), FindSamplePosition_hook, old_FindSamplePosition);      
   SendToServerPostMessage = (t_SendToServerPostMessage)(libBase + str2Offset("0x403cc18"));
    
   HOOK(targetLibName, str2Offset("0x403cc18"), SendToServerPostMessage_hook, old_SendToServerPostMessage);
  HOOK(targetLibName, str2Offset("0x403de54"), ctor_hook, old_ctor);
    
    
  //  Patch Anticheat
  
    // Khai báo offset


patchReturnFalse(libBase + 0x36e8e28); // get_IsCheating() → trả false
patchReturnFalse(libBase + 0x36e8e78); // CheckDetect(HType) → trả false

patchNOP(libBase + 0x36e8f1c);  // OnSpeedHackDetected() → NOP
patchNOP(libBase + 0x36e8fa4);  // OnSpeedHackDetectedGpresto() → NOP
patchNOP(libBase + 0x36e9038);  // OnTimeCheatingDetected() → NOP
patchNOP(libBase + 0x36e9040);  // OnObscuredTypeCheatingDetected() → NOP
patchNOP(libBase + 0x36e90c8);  // OnTableCheatingDetected(string) → NOP
patchNOP(libBase + 0x36e9128);  // OnOtherCheatingDetected(HType,string) → NOP
patchNOP(libBase + 0x36e91a4);  // OnAnimationSpeedCheatDetect(...) → NOP
patchNOP(libBase + 0x36e94c4);  // LateUpdate() → NOP
// Gọi patch
return nullptr;
}


__attribute__((constructor))
void lib_main() {
    pthread_t t;
    pthread_create(&t, nullptr, hack_thread, nullptr);
}
