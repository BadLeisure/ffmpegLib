#include <jni.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <android/log.h>

extern "C" {
#include "include/libavformat/avformat.h"
}

#include "RtspPlayer.h"
#ifdef LOGD
#undef LOGD
#endif

#define LOG_TAG "NativeBridge"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace {
constexpr const char* kMainActivityClassPath = "org/easydarwin/sw/FFmpegUtil";
constexpr const char* kCallbackSignature = "(Lorg/easydarwin/sw/FFmpegCallback;)J";

JavaVM* gJvm = nullptr;
std::mutex gRegistryMutex;
std::atomic<int64_t> gNextHandle{1};
std::unordered_map<int64_t, std::shared_ptr<RtspPlayer>> gPlayers;

std::shared_ptr<RtspPlayer> findPlayerLocked(int64_t handle) {
    auto it = gPlayers.find(handle);
    if (it == gPlayers.end()) {
        LOGD("player not found, handle=%lld", static_cast<long long>(handle));
        return nullptr;
    }
    return it->second;
}

extern "C" JNIEXPORT jlong JNICALL nativeCreatePlayer(JNIEnv* env, jobject thiz, jobject callback) {
    (void)thiz;
    //从原子计数器 gNextHandle 里取出当前值，作为这次播放器实例的唯一 handle，然后再把计数器加 1
    const int64_t handle = gNextHandle.fetch_add(1);
    auto player = std::make_shared<RtspPlayer>(gJvm);
    player->bindCallback(env, callback);
    {
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        gPlayers[handle] = player;
    }
    LOGD("create player, handle=%lld", static_cast<long long>(handle));
    return static_cast<jlong>(handle);
}

extern "C" JNIEXPORT void JNICALL nativeStartRtsp(JNIEnv* env, jobject thiz, jlong handle, jstring jurl) {
    (void)thiz;
    std::shared_ptr<RtspPlayer> player;
    {
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        player = findPlayerLocked(static_cast<int64_t>(handle));
    }
    if (!player) {
        LOGD("startRtsp ignored because player is not initialized, handle=%lld",
             static_cast<long long>(handle));
        return;
    }

    const char* url = env->GetStringUTFChars(jurl, nullptr);
    std::string urlString = url ? url : "";
    if (url) {
        env->ReleaseStringUTFChars(jurl, url);
    }
    player->start(urlString);
}

extern "C" JNIEXPORT void JNICALL nativeStopPlayer(JNIEnv* env, jobject thiz, jlong handle) {
    (void)env;
    (void)thiz;
    std::shared_ptr<RtspPlayer> player;
    {
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        player = findPlayerLocked(static_cast<int64_t>(handle));
    }
    if (!player) {
        return;
    }
    LOGD("call to stop");
    player->stop();
}

extern "C" JNIEXPORT void JNICALL nativeDestroyPlayer(JNIEnv* env, jobject thiz, jlong handle) {
    (void)env;
    (void)thiz;
    std::shared_ptr<RtspPlayer> player;
    {
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        auto it = gPlayers.find(static_cast<int64_t>(handle));
        if (it == gPlayers.end()) {
            return;
        }
        // destroy 先把 handle 从注册表移除，后续外部就不能再通过这个 handle 取到 player。
        player = it->second;
        gPlayers.erase(it);
    }
    if (player) {
        LOGD("destroy player, handle=%lld", static_cast<long long>(handle));
        // 真正的 callback/global ref/native 资源回收由 RtspPlayer 析构统一处理。
        player->requestStop();
    }
}

int registerNativeMethods(JNIEnv* env, const char* className, JNINativeMethod* methods, int numMethods) {
    jclass clazz = env->FindClass(className);
    if (clazz == nullptr) {
        return JNI_FALSE;
    }

    if (env->RegisterNatives(clazz, methods, numMethods) < 0) {
        LOGD("RegisterNatives failed");
        env->DeleteLocalRef(clazz);
        return JNI_FALSE;
    }

    env->DeleteLocalRef(clazz);
    return JNI_TRUE;
}
}

static JNINativeMethod kMethods[] = {
        {"createPlayer",  kCallbackSignature,               reinterpret_cast<void*>(nativeCreatePlayer)},
        {"startRtsp",     "(JLjava/lang/String;)V",         reinterpret_cast<void*>(nativeStartRtsp)},
        {"stopPlayer",    "(J)V",                           reinterpret_cast<void*>(nativeStopPlayer)},
        {"destroyPlayer", "(J)V",                           reinterpret_cast<void*>(nativeDestroyPlayer)},};

jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    gJvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return -1;
    }

    if (!registerNativeMethods(env, kMainActivityClassPath, kMethods, sizeof(kMethods) / sizeof(kMethods[0]))) {
        return -1;
    }

    avformat_network_init();

    return JNI_VERSION_1_6;
}
