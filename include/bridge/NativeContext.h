#ifndef SPECTRA_BRIDGE_BRIDGE_NATIVE_CONTEXT_H
#define SPECTRA_BRIDGE_BRIDGE_NATIVE_CONTEXT_H

#include <memory>

#include "bridge/JniBridge.h"
#include "service/SpectraBridgeClient.h"

namespace spectra {
namespace bridge {

// 一个 Java SpectraBridgeNative 对象在 C++ 侧对应一个 NativeContext。
//
// 这里把 JNI 回调桥和业务 client 放在同一个上下文里，便于：
// 1. Java 用 long nativeHandle 管理 native 生命周期；
// 2. nativeDestroy 时一次性正确释放所有资源。
struct NativeContext {
    explicit NativeContext(std::unique_ptr<JniBridge> bridge_in)
        : bridge(std::move(bridge_in)),
          client(new service::SpectraBridgeClient(bridge.get()))
    {
    }

    std::unique_ptr<JniBridge> bridge;
    std::unique_ptr<service::SpectraBridgeClient> client;
};

}  // namespace bridge
}  // namespace spectra

#endif  // SPECTRA_BRIDGE_BRIDGE_NATIVE_CONTEXT_H
