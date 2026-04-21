#ifndef SPECTRA_BRIDGE_BRIDGE_CALLBACKS_H
#define SPECTRA_BRIDGE_BRIDGE_CALLBACKS_H

#include <string>

#include "image/ImageConverter.h"
#include "protocol/ControlProtocol.h"

namespace spectra {
namespace bridge {

// service 层和未来 JNI 层之间的抽象边界。
// 这样网络采集逻辑不需要直接依赖 jni.h，后续替换为真实 JNI 实现即可。
class BridgeCallbacks {
public:
    virtual ~BridgeCallbacks() {}

    // 图像接收线程在完成“16 位容器、低 12 位有效” -> 16bit/8bit 转换后回调。
    virtual void OnImageFrameReady(const image::ConvertedImageFrame& frame) = 0;
    // 控制接收线程收到状态包后回调。
    virtual void OnStatusPacket(const protocol::StatusPacket& packet) = 0;
    // 控制接收线程收到配置应答包后回调。
    virtual void OnConfigAckPacket(const protocol::ConfigAckPacket& packet) = 0;
    // 任一链路出错时回调，便于上层统一做重连或错误提示。
    virtual void OnTransportError(const std::string& channel, const std::string& message) = 0;
};

}  // namespace bridge
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_BRIDGE_CALLBACKS_H
