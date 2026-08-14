

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <jni.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <atomic>
#include <cstdio>

#pragma pack(push, 8)
struct NickIpc {
    volatile LONG seq;
    volatile LONG ack;
    volatile LONG cmd;
    volatile LONG result;
    char nick[32];
};
#pragma pack(pop)

enum {
    CMD_NONE = 0,
    CMD_SET_NICK = 1,
    CMD_PREMIUM = 2
};

enum {
    RES_IDLE = 0,
    RES_OK = 1,
    RES_FAIL = 2
};

static const char* kIpcName = "Local\\LunarNickOverlayIpc";

static std::atomic<bool> g_running{true};
static HMODULE g_hModule = nullptr;
static JavaVM* g_jvm = nullptr;
static jobject g_gameLoader = nullptr;
static jobject g_premiumSession = nullptr;
static HANDLE g_map = nullptr;
static NickIpc* g_ipc = nullptr;

void LogLine(const char* text) {
    char tempPath[MAX_PATH] = {0};
    if (!GetTempPathA(MAX_PATH, tempPath)) return;
    char path[MAX_PATH] = {0};
    wsprintfA(path, "%slunar_overlay_log.txt", tempPath);
    FILE* f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u] %s\n", st.wHour, st.wMinute, st.wSecond, text ? text : "");
    fclose(f);
}

static bool JniCheck(JNIEnv* env, const char* step) {
    if (!env->ExceptionCheck()) return true;
    env->ExceptionClear();
    char msg[128];
    wsprintfA(msg, "JNI fail: %s", step);
    LogLine(msg);
    return false;
}

static bool GetJNIEnv(JNIEnv** outEnv) {
    if (!g_jvm) {
        HMODULE jvmMod = GetModuleHandleA("jvm.dll");
        if (!jvmMod) return false;
        auto getVMs = reinterpret_cast<jint (JNICALL *)(JavaVM**, jsize, jsize*)>(
            GetProcAddress(jvmMod, "JNI_GetCreatedJavaVMs"));
        if (!getVMs) return false;
        JavaVM* vms[1];
        jsize n = 0;
        if (getVMs(vms, 1, &n) != JNI_OK || n == 0) return false;
        g_jvm = vms[0];
    }
    jint st = g_jvm->GetEnv(reinterpret_cast<void**>(outEnv), JNI_VERSION_1_8);
    if (st == JNI_EDETACHED) {
        return g_jvm->AttachCurrentThread(reinterpret_cast<void**>(outEnv), nullptr) == JNI_OK;
    }
    return st == JNI_OK;
}

static jstring MakeJString(JNIEnv* env, const char* utf8) {
    return utf8 ? env->NewStringUTF(utf8) : nullptr;
}

static jclass LoadWithLoader(JNIEnv* env, jobject loader, const char* slashName) {
    if (!loader) return nullptr;
    jclass loaderCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = env->GetMethodID(loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!loadClass) {
        env->ExceptionClear();
        return nullptr;
    }
    std::string dotName = slashName;
    for (char& c : dotName) if (c == '/') c = '.';
    jstring nameJ = env->NewStringUTF(dotName.c_str());
    jobject clsObj = env->CallObjectMethod(loader, loadClass, nameJ);
    env->DeleteLocalRef(nameJ);
    if (!clsObj || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return (jclass)clsObj;
}

static jobject FindLaunchClassLoader(JNIEnv* env) {
    jclass launchCls = env->FindClass("net/minecraft/launchwrapper/Launch");
    if (!launchCls) {
        env->ExceptionClear();
        jclass threadCls = env->FindClass("java/lang/Thread");
        jmethodID getAll = env->GetStaticMethodID(threadCls, "getAllStackTraces", "()Ljava/util/Map;");
        jmethodID getCtx = env->GetMethodID(threadCls, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
        if (getAll && getCtx) {
            jobject mapObj = env->CallStaticObjectMethod(threadCls, getAll);
            if (mapObj && !env->ExceptionCheck()) {
                jclass mapCls = env->FindClass("java/util/Map");
                jmethodID keySet = env->GetMethodID(mapCls, "keySet", "()Ljava/util/Set;");
                jobject setObj = env->CallObjectMethod(mapObj, keySet);
                jclass setCls = env->FindClass("java/util/Set");
                jmethodID toArray = env->GetMethodID(setCls, "toArray", "()[Ljava/lang/Object;");
                jobjectArray threads = (jobjectArray)env->CallObjectMethod(setObj, toArray);
                if (threads && !env->ExceptionCheck()) {
                    jsize count = env->GetArrayLength(threads);
                    for (jsize i = 0; i < count && !launchCls; ++i) {
                        jobject thr = env->GetObjectArrayElement(threads, i);
                        if (!thr) continue;
                        jobject loader = env->CallObjectMethod(thr, getCtx);
                        if (!loader || env->ExceptionCheck()) {
                            env->ExceptionClear();
                            continue;
                        }
                        launchCls = LoadWithLoader(env, loader, "net/minecraft/launchwrapper/Launch");
                    }
                }
            }
            env->ExceptionClear();
        }
    }
    if (!launchCls) return nullptr;
    jfieldID clField = env->GetStaticFieldID(launchCls, "classLoader", "Lnet/minecraft/launchwrapper/LaunchClassLoader;");
    if (!clField) {
        env->ExceptionClear();
        return nullptr;
    }
    jobject loader = env->GetStaticObjectField(launchCls, clField);
    if (!loader || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return loader;
}

static jobject FindGameClassLoader(JNIEnv* env) {
    if (g_gameLoader) return g_gameLoader;

    jobject launchLoader = FindLaunchClassLoader(env);
    if (launchLoader) {
        jclass probe = LoadWithLoader(env, launchLoader, "net/minecraft/client/Minecraft");
        if (probe) {
            env->DeleteLocalRef(probe);
            g_gameLoader = env->NewGlobalRef(launchLoader);
            LogLine("ClassLoader: Launch.classLoader");
            return g_gameLoader;
        }
    }

    jclass threadCls = env->FindClass("java/lang/Thread");
    jmethodID getAll = env->GetStaticMethodID(threadCls, "getAllStackTraces", "()Ljava/util/Map;");
    jmethodID getCtx = env->GetMethodID(threadCls, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    if (!getAll || !getCtx) {
        env->ExceptionClear();
        return nullptr;
    }
    jobject mapObj = env->CallStaticObjectMethod(threadCls, getAll);
    if (!mapObj || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    jclass mapCls = env->FindClass("java/util/Map");
    jmethodID keySet = env->GetMethodID(mapCls, "keySet", "()Ljava/util/Set;");
    jobject setObj = env->CallObjectMethod(mapObj, keySet);
    jclass setCls = env->FindClass("java/util/Set");
    jmethodID toArray = env->GetMethodID(setCls, "toArray", "()[Ljava/lang/Object;");
    jobjectArray threads = (jobjectArray)env->CallObjectMethod(setObj, toArray);
    if (!threads || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }

    jsize count = env->GetArrayLength(threads);
    for (jsize i = 0; i < count; ++i) {
        jobject thr = env->GetObjectArrayElement(threads, i);
        if (!thr) continue;
        jobject loader = env->CallObjectMethod(thr, getCtx);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            continue;
        }
        if (!loader) continue;
        jclass probe = LoadWithLoader(env, loader, "net/minecraft/client/Minecraft");
        if (probe) {
            env->DeleteLocalRef(probe);
            g_gameLoader = env->NewGlobalRef(loader);
            LogLine("ClassLoader: thread context");
            return g_gameLoader;
        }
    }
    LogLine("ClassLoader: not found");
    return nullptr;
}

static jclass LoadClass(JNIEnv* env, const char* slashName) {
    jclass cls = env->FindClass(slashName);
    if (cls) return cls;
    env->ExceptionClear();
    return LoadWithLoader(env, FindGameClassLoader(env), slashName);
}

static std::string OfflinePlayerId(JNIEnv* env, const char* username) {
    std::string prefix = "OfflinePlayer:";
    prefix += username;
    jclass uuidCls = env->FindClass("java/util/UUID");
    if (!uuidCls) return "";
    jmethodID fromBytes = env->GetStaticMethodID(uuidCls, "nameUUIDFromBytes", "([B)Ljava/util/UUID;");
    if (!fromBytes) return "";
    jbyteArray arr = env->NewByteArray((jsize)prefix.size());
    env->SetByteArrayRegion(arr, 0, (jsize)prefix.size(), reinterpret_cast<const jbyte*>(prefix.data()));
    jobject uuidObj = env->CallStaticObjectMethod(uuidCls, fromBytes, arr);
    env->DeleteLocalRef(arr);
    if (!uuidObj || !JniCheck(env, "nameUUIDFromBytes")) return "";
    jmethodID toString = env->GetMethodID(uuidCls, "toString", "()Ljava/lang/String;");
    auto uuidJ = (jstring)env->CallObjectMethod(uuidObj, toString);
    if (!uuidJ || !JniCheck(env, "uuid.toString")) return "";
    const char* uuidC = env->GetStringUTFChars(uuidJ, nullptr);
    std::string out = uuidC ? uuidC : "";
    if (uuidC) env->ReleaseStringUTFChars(uuidJ, uuidC);
    std::string compact;
    for (char c : out) if (c != '-') compact.push_back(c);
    return compact;
}

static jobject GetMinecraft(JNIEnv* env) {
    jclass mcCls = LoadClass(env, "net/minecraft/client/Minecraft");
    if (!mcCls) return nullptr;
    jmethodID getMc = env->GetStaticMethodID(mcCls, "getMinecraft", "()Lnet/minecraft/client/Minecraft;");
    if (!getMc) {
        env->ExceptionClear();
        return nullptr;
    }
    jobject mcObj = env->CallStaticObjectMethod(mcCls, getMc);
    if (!mcObj || !JniCheck(env, "getMinecraft")) return nullptr;
    return mcObj;
}

static bool SetSessionOnMinecraft(JNIEnv* env, jobject mcObj, jobject sessionObj) {
    jclass mcCls = env->GetObjectClass(mcObj);
    if (!mcCls) return false;
    jmethodID setSession = env->GetMethodID(mcCls, "bridge$setSession", "(Lnet/minecraft/util/Session;)V");
    if (!setSession) {
        env->ExceptionClear();
        setSession = env->GetMethodID(mcCls, "setSession", "(Lnet/minecraft/util/Session;)V");
    }
    if (setSession) {
        env->CallVoidMethod(mcObj, setSession, sessionObj);
        if (JniCheck(env, "setSession")) return true;
    }
    jfieldID sessionField = env->GetFieldID(mcCls, "session", "Lnet/minecraft/util/Session;");
    if (!sessionField) {
        env->ExceptionClear();
        return false;
    }
    env->SetObjectField(mcObj, sessionField, sessionObj);
    return JniCheck(env, "session field");
}

static jobject GetCurrentSession(JNIEnv* env, jobject mcObj) {
    jclass mcCls = env->GetObjectClass(mcObj);
    jmethodID getSession = env->GetMethodID(mcCls, "getSession", "()Lnet/minecraft/util/Session;");
    if (!getSession) {
        env->ExceptionClear();
        getSession = env->GetMethodID(mcCls, "bridge$getSession", "()Lnet/minecraft/util/Session;");
    }
    if (getSession) {
        jobject s = env->CallObjectMethod(mcObj, getSession);
        if (s && JniCheck(env, "getSession")) return s;
    }
    jfieldID sessionField = env->GetFieldID(mcCls, "session", "Lnet/minecraft/util/Session;");
    if (!sessionField) {
        env->ExceptionClear();
        return nullptr;
    }
    return env->GetObjectField(mcObj, sessionField);
}

static bool CapturePremiumSession() {
    JNIEnv* env = nullptr;
    if (!GetJNIEnv(&env)) return false;
    jobject mcObj = GetMinecraft(env);
    if (!mcObj) return false;
    jobject session = GetCurrentSession(env, mcObj);
    if (!session) {
        LogLine("CapturePremium: no session");
        return false;
    }
    if (g_premiumSession) {
        env->DeleteGlobalRef(g_premiumSession);
        g_premiumSession = nullptr;
    }
    g_premiumSession = env->NewGlobalRef(session);
    LogLine("CapturePremium: ok");
    return g_premiumSession != nullptr;
}

static bool RestorePremium() {
    if (!g_premiumSession) {
        LogLine("RestorePremium: no saved session");
        return false;
    }
    JNIEnv* env = nullptr;
    if (!GetJNIEnv(&env)) return false;
    jobject mcObj = GetMinecraft(env);
    if (!mcObj) return false;
    jobject current = GetCurrentSession(env, mcObj);
    if (current && env->IsSameObject(current, g_premiumSession)) {
        LogLine("RestorePremium: already premium");
        return true;
    }
    if (!SetSessionOnMinecraft(env, mcObj, g_premiumSession)) {
        LogLine("RestorePremium: set failed");
        return false;
    }
    LogLine("RestorePremium: ok");
    return true;
}

static bool ApplyOfflineNick(const std::string& username) {
    if (username.empty() || username.size() > 16) return false;
    JNIEnv* env = nullptr;
    if (!GetJNIEnv(&env)) {
        LogLine("ApplyOfflineNick: no JVM");
        return false;
    }
    jobject mcObj = GetMinecraft(env);
    if (!mcObj) {
        LogLine("ApplyOfflineNick: Minecraft missing");
        return false;
    }
    jclass sessionCls = LoadClass(env, "net/minecraft/util/Session");
    if (!sessionCls) {
        LogLine("ApplyOfflineNick: Session class missing");
        return false;
    }
    std::string playerId = OfflinePlayerId(env, username.c_str());
    if (playerId.empty()) return false;

    jstring nameJ = MakeJString(env, username.c_str());
    jstring idJ = MakeJString(env, playerId.c_str());
    jstring tokenJ = env->NewStringUTF("0");
    jobject sessionObj = nullptr;

    jmethodID sessionInitStr = env->GetMethodID(sessionCls, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (sessionInitStr) {
        jstring typeStr = env->NewStringUTF("legacy");
        sessionObj = env->NewObject(sessionCls, sessionInitStr, nameJ, idJ, tokenJ, typeStr);
        if (!JniCheck(env, "new Session(str)")) sessionObj = nullptr;
    } else {
        env->ExceptionClear();
    }

    if (!sessionObj) {
        jclass typeCls = LoadClass(env, "net/minecraft/util/Session$Type");
        jmethodID sessionInitType = env->GetMethodID(sessionCls, "<init>",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lnet/minecraft/util/Session$Type;)V");
        if (!typeCls || !sessionInitType) return false;
        jfieldID legacyType = env->GetStaticFieldID(typeCls, "LEGACY", "Lnet/minecraft/util/Session$Type;");
        if (!legacyType) {
            env->ExceptionClear();
            legacyType = env->GetStaticFieldID(typeCls, "OFFLINE", "Lnet/minecraft/util/Session$Type;");
        }
        if (!legacyType) return false;
        jobject typeObj = env->GetStaticObjectField(typeCls, legacyType);
        sessionObj = env->NewObject(sessionCls, sessionInitType, nameJ, idJ, tokenJ, typeObj);
        if (!sessionObj || !JniCheck(env, "new Session(type)")) return false;
    }

    if (!SetSessionOnMinecraft(env, mcObj, sessionObj)) return false;
    LogLine("ApplyOfflineNick: ok");
    return true;
}

static bool OpenIpcServer() {
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(NickIpc), kIpcName);
    if (!g_map) {
        LogLine("IPC: CreateFileMapping failed");
        return false;
    }
    g_ipc = (NickIpc*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NickIpc));
    if (!g_ipc) {
        LogLine("IPC: MapViewOfFile failed");
        CloseHandle(g_map);
        g_map = nullptr;
        return false;
    }
    ZeroMemory((void*)g_ipc, sizeof(NickIpc));
    LogLine("IPC: ready");
    return true;
}

static void WorkerThread() {
    LogLine("WorkerThread: started");
    while (g_running && !CapturePremiumSession()) Sleep(500);
    if (!g_running) return;

    LONG last = 0;
    while (g_running) {
        if (g_ipc) {
            LONG seq = InterlockedCompareExchange(&g_ipc->seq, 0, 0);
            if (seq > last) {
                LONG cmd = InterlockedCompareExchange(&g_ipc->cmd, 0, 0);
                char nick[32] = {0};
                memcpy(nick, (const void*)g_ipc->nick, 31);
                bool ok = false;
                if (cmd == CMD_SET_NICK) {
                    ok = ApplyOfflineNick(nick);
                } else if (cmd == CMD_PREMIUM) {
                    ok = RestorePremium();
                }
                InterlockedExchange(&g_ipc->result, ok ? RES_OK : RES_FAIL);
                MemoryBarrier();
                InterlockedExchange(&g_ipc->ack, seq);
                last = seq;
            }
        }
        Sleep(20);
    }
}

DWORD WINAPI InitThread(LPVOID) {
    if (!OpenIpcServer()) return 0;
    WorkerThread();
    if (g_ipc) {
        UnmapViewOfFile((LPCVOID)g_ipc);
        g_ipc = nullptr;
    }
    if (g_map) {
        CloseHandle(g_map);
        g_map = nullptr;
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LogLine("DllMain: attach");
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE h = CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        LogLine("DllMain: detach");
        g_running = false;
    }
    return TRUE;
}
