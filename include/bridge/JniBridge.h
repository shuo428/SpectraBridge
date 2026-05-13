#ifndef SPECTRA_BRIDGE_BRIDGE_JNI_BRIDGE_H
#define SPECTRA_BRIDGE_BRIDGE_JNI_BRIDGE_H

#include <jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "bridge/BridgeCallbacks.h"

namespace spectra {
namespace bridge {

// 真正的 JNI 回调桥实现。
//
// 职责：
// 1. 持有 JavaVM 指针和 BridgeListener 的全局引用；
// 2. 在 C++ 后台线程中安全附着到 JVM；
// 3. 把图像、状态、配置结果和错误回调转发给 Java。
class JniBridge : public BridgeCallbacks {
public:
    // 通过工厂方法创建，便于在初始化失败时返回详细错误信息。
    static std::unique_ptr<JniBridge> Create(JNIEnv* env, jobject listener, std::string* error);

    ~JniBridge() override;

    JniBridge(const JniBridge&) = delete;
    JniBridge& operator=(const JniBridge&) = delete;

    void OnImageFrameReady(const image::ConvertedImageFrame& frame) override;
    void OnStatusPacket(const protocol::StatusPacket& packet) override;
    void OnConfigAckPacket(const protocol::ConfigAckPacket& packet) override;
    void OnTransportError(const std::string& channel, const std::string& message) override;

private:
    explicit JniBridge(JavaVM* java_vm);

    bool Initialize(JNIEnv* env, jobject listener, std::string* error);

    // 获取当前线程可用的 JNIEnv。
    // 如果当前线程尚未附着到 JVM，会自动执行 AttachCurrentThread。
    bool AcquireThreadEnv(JNIEnv** env, bool* did_attach, std::string* error) const;
    void ReleaseThreadEnv(bool did_attach) const;

    // C++ 后台线程中的 Java 异常无法像普通 JNI 调用那样向上抛回 Java 调用栈，
    // 因此这里统一打印并清理，避免线程进入异常污染状态。
    static void LogAndClearJavaException(JNIEnv* env);

    JavaVM* java_vm_;
    jobject listener_global_ref_;
    jmethodID on_image_frame_method_;
    jmethodID on_status_method_;
    jmethodID on_config_ack_method_;
    jmethodID on_transport_error_method_;

    // 图像线程和控制线程都可能并发回调同一个 Java listener。
    // 这里串行化 JNI 回调，避免数组创建和 Java 回调交错执行。
    std::mutex callback_mutex_;
};

}  // namespace bridge
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_BRIDGE_JNI_BRIDGE_H
