package com.spectrabridge.jni;

import java.util.Objects;

/**
 * Java 到 C++ JNI 桥接入口。
 *
 * <p>这层类的职责是：
 * <p>1. 加载 native 动态库；
 * <p>2. 持有一个 nativeHandle，对应 C++ 侧的 NativeContext；
 * <p>3. 把 Java 的连接/控制调用转发给 C++；
 * <p>4. 通过 BridgeListener 接收 C++ 的异步回调。
 *
 * <p>建议的 native 实现策略：
 * <p>1. nativeCreate(listener) 在 C++ 中创建 NativeContext；
 * <p>2. NativeContext 内部持有 SpectraBridgeClient 和 JNI 回调桥对象；
 * <p>3. nativeDestroy(handle) 负责断开连接、释放全局引用并销毁上下文。
 *
 * <p>生成 JNI 头文件时，可以对当前包执行类似命令：
 * <pre>{@code
 * javac -encoding UTF-8 -h generated src/main/java/com/spectrabridge/jni/*.java
 * }</pre>
 */
public final class SpectraBridgeNative implements AutoCloseable {

    /**
     * 这里的库名只是设计建议。
     * 真正落地时需要和 CMake/Gradle 产出的动态库名称保持一致。
     */
    private static final String DEFAULT_LIBRARY_NAME = "SpectraBridgeJni";

    /**
     * 完整配置包固定为 512 字节。
     * 这里和 C++ 侧 protocol::kFullConfigPayloadSize 保持一致。
     */
    public static final int FULL_CONFIG_SIZE = 512;

    static {
        System.loadLibrary(DEFAULT_LIBRARY_NAME);
    }

    private final Object stateLock = new Object();
    private final BridgeListener listener;
    private long nativeHandle;

    /**
     * 创建一个新的 Java/native 桥接实例。
     *
     * <p>构造时会立即进入 nativeCreate，让 C++ 侧创建配套的 NativeContext。
     * 这样后续所有 JNI 调用都可以通过 nativeHandle 找到对应上下文。
     */
    public SpectraBridgeNative(BridgeListener listener) {
        this.listener = Objects.requireNonNull(listener, "listener must not be null");
        this.nativeHandle = nativeCreate(this.listener);
        if (this.nativeHandle == 0L) {
            throw new IllegalStateException("nativeCreate returned an invalid handle");
        }
    }

    /**
     * 建立到 FPGA 的控制 TCP 和图像 TCP。
     *
     * <p>这里约定两个 TCP 使用同一个 host，但端口分别独立。
     * 如果你的部署需要控制通道和图像通道走不同 IP，可以把签名再拆开。
     */
    public void connect(String host, int controlPort, int imagePort, boolean verifyCrc) {
        Objects.requireNonNull(host, "host must not be null");
        validatePort(controlPort, "controlPort");
        validatePort(imagePort, "imagePort");

        nativeConnect(requireHandle(), host, controlPort, imagePort, verifyCrc);
    }

    /**
     * 主动断开 native 侧 TCP 连接。
     *
     * <p>这个方法可以安全重复调用；真正的资源释放仍建议通过 close() 完成。
     */
    public void disconnect() {
        synchronized (stateLock) {
            if (nativeHandle == 0L) {
                return;
            }
            nativeDisconnect(nativeHandle);
        }
    }

    /**
     * 发送复位命令。
     */
    public void sendReset() {
        nativeSendReset(requireHandle());
    }

    /**
     * 发送单次触发命令。
     */
    public void sendTriggerOnce() {
        nativeSendTriggerOnce(requireHandle());
    }

    /**
     * 发送状态查询命令。
     *
     * <p>查询结果不会同步返回，而是稍后通过 listener.onStatus(...) 回调。
     */
    public void sendQueryStatus() {
        nativeSendQueryStatus(requireHandle());
    }

    /**
     * 下发完整配置寄存器镜像。
     *
     * <p>配置结果不会同步返回，而是稍后通过 listener.onConfigAck(...) 回调。
     */
    public void sendFullConfig(byte[] regs512) {
        Objects.requireNonNull(regs512, "regs512 must not be null");
        if (regs512.length != FULL_CONFIG_SIZE) {
            throw new IllegalArgumentException(
                "regs512 length must be exactly " + FULL_CONFIG_SIZE + " bytes");
        }

        nativeSendFullConfig(requireHandle(), regs512);
    }

    /**
     * 返回当前对象是否已经释放掉 native 侧上下文。
     */
    public boolean isClosed() {
        synchronized (stateLock) {
            return nativeHandle == 0L;
        }
    }

    /**
     * 释放 native 资源。
     *
     * <p>close() 会调用 nativeDestroy(handle)，通常它应该做三件事：
     * <p>1. 停掉 SpectraBridgeClient；
     * <p>2. 释放 BridgeListener 的 JNI 全局引用；
     * <p>3. 删除 NativeContext。
     */
    @Override
    public void close() {
        synchronized (stateLock) {
            if (nativeHandle == 0L) {
                return;
            }

            nativeDestroy(nativeHandle);
            nativeHandle = 0L;
        }
    }

    /**
     * 保留对监听器的强引用，避免上层只持有 SpectraBridgeNative 时，
     * listener 过早被 Java GC 回收。
     */
    public BridgeListener getListener() {
        return listener;
    }

    private long requireHandle() {
        synchronized (stateLock) {
            if (nativeHandle == 0L) {
                throw new IllegalStateException("native handle has already been destroyed");
            }
            return nativeHandle;
        }
    }

    private static void validatePort(int port, String fieldName) {
        if (port < 1 || port > 65535) {
            throw new IllegalArgumentException(fieldName + " must be in range 1..65535");
        }
    }

    /**
     * 创建 C++ 侧 NativeContext，并返回其地址转成的 long handle。
     */
    private static native long nativeCreate(BridgeListener listener);

    /**
     * 销毁 C++ 侧 NativeContext。
     */
    private static native void nativeDestroy(long nativeHandle);

    /**
     * 建立控制 TCP 和图像 TCP 连接。
     */
    private static native void nativeConnect(
        long nativeHandle,
        String host,
        int controlPort,
        int imagePort,
        boolean verifyCrc);

    /**
     * 主动断开两条 TCP 连接。
     */
    private static native void nativeDisconnect(long nativeHandle);

    /**
     * 发送 FPGA 复位命令。
     */
    private static native void nativeSendReset(long nativeHandle);

    /**
     * 发送单帧触发命令。
     */
    private static native void nativeSendTriggerOnce(long nativeHandle);

    /**
     * 发送状态查询命令。
     */
    private static native void nativeSendQueryStatus(long nativeHandle);

    /**
     * 发送 512 字节完整配置包。
     */
    private static native void nativeSendFullConfig(long nativeHandle, byte[] regs512);
}
