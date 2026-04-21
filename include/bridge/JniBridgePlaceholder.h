#ifndef SPECTRA_BRIDGE_BRIDGE_JNI_BRIDGE_PLACEHOLDER_H
#define SPECTRA_BRIDGE_BRIDGE_JNI_BRIDGE_PLACEHOLDER_H

#include <mutex>

#include "bridge/BridgeCallbacks.h"

namespace spectra {
namespace bridge {

// JNI 占位实现。
// 当前用日志代替真正的 Java 回调，后续接入 jni.h 时直接替换方法体即可。
class JniBridgePlaceholder : public BridgeCallbacks {
public:
    void OnImageFrameReady(const image::ConvertedImageFrame& frame) override;
    void OnStatusPacket(const protocol::StatusPacket& packet) override;
    void OnConfigAckPacket(const protocol::ConfigAckPacket& packet) override;
    void OnTransportError(const std::string& channel, const std::string& message) override;

private:
    // 这里只保护输出，避免两个后台线程同时打印时日志交错。
    std::mutex print_mutex_;
};

}  // namespace bridge
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_BRIDGE_JNI_BRIDGE_PLACEHOLDER_H
