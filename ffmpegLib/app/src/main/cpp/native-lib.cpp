#include <jni.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

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

extern "C" JNIEXPORT jboolean JNICALL nativePauseAllRtsp(JNIEnv* env, jobject thiz) {
    (void)env;
    (void)thiz;

    //复制播放器列表，gPlayers是native 层维护的所有播放器
    std::vector<std::shared_ptr<RtspPlayer>> players;
    {
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        players.reserve(gPlayers.size());
        for (const auto& entry : gPlayers) {
            players.push_back(entry.second);
        }
    }


    for (const auto& player : players) {
        //每个播放器都请求暂停，主要是“发出停止请求”，不一定已经完全停止
        player->pauseForBandwidth();
    }


    bool allStopped = true;
    for (const auto& player : players) {
        if (!player->waitUntilIdle(5000)) {
            allStopped = false;
            LOGD("pauseAllRtsp timeout waiting for player to stop");
        }
    }
    //所有播放器都停止成功 -> true
    return allStopped ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL nativeResumeAllRtsp(JNIEnv* env, jobject thiz) {
    (void)env;
    (void)thiz;

    std::vector<std::shared_ptr<RtspPlayer>> players;
    {
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        players.reserve(gPlayers.size());
        for (const auto& entry : gPlayers) {
            players.push_back(entry.second);
        }
    }

    for (const auto& player : players) {
        player->resumeAfterBandwidth();
    }
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
        {"pauseAllRtsp",  "()Z",                            reinterpret_cast<void*>(nativePauseAllRtsp)},
        {"resumeAllRtsp", "()V",                            reinterpret_cast<void*>(nativeResumeAllRtsp)},
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
