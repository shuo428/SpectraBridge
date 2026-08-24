package com.spectrabridge.jni;

/**
 * Native 侧通过 JNI 回调到 Java 的监听接口。
 *
 * <p>注意事项：
 * <p>1. 这些回调通常来自 C++ 后台线程，而不是 Java 主线程/UI 线程。
 * <p>2. 如果你的上层是 Swing/JavaFX/Android，需要自行切回 UI 线程再更新界面。
 * <p>3. 图像回调会比较频繁，回调里不要做阻塞时间过长的操作。
 */
public interface BridgeListener {

    /**
     * 收到一帧图像后的回调。
     *
     * @param width 图像宽度
     * @param height 图像高度
     * @param pixels16 16 位灰度数组，长度应为 width * height。
     *                 当前协议下每个元素只有低 12 位是有效像素值，范围 0~4095。
     * @param pixels8 8 位灰度数组，长度应为 width * height。
     *                这是 native 侧把低 12 位有效值缩放到 0~255 后得到的显示图。
     * @param fpgaPayload FPGA 图像通道直接收到的原始有效像素 payload。
     *                    它保持芯片/FPGA读出顺序，不做 GLUX1605BSI HDR 4-lane 转序。
     */
    void onImageFrame(int width, int height, short[] pixels16, byte[] pixels8, byte[] fpgaPayload);

    /**
     * 收到 HDR 双增益图像后的回调。
     *
     * @param width 图像宽度
     * @param height 图像高度
     * @param hgPixels16 高增益平面，已经重排为正常行列顺序
     * @param lgPixels16 低增益平面，已经重排为正常行列顺序
     * @param fpgaPayload FPGA 原始双平面 payload，顺序为 HG + LG
     */
    void onHdrImageFrame(int width, int height, short[] hgPixels16, short[] lgPixels16, byte[] fpgaPayload);

    /**
     * 收到状态包后的回调。
     *
     * @param statusBits FPGA 返回的状态位
     * @param errorCode FPGA 返回的错误码
     */
    void onStatus(int statusBits, int errorCode);

    /**
     * 收到配置结果包后的回调。
     *
     * @param resultCode 结果码，0 通常表示成功
     * @param failedAddr 失败地址，仅失败时有业务意义
     */
    void onConfigAck(int resultCode, int failedAddr);

    /**
     * native 传输层发生错误时的统一回调。
     *
     * @param channel 出错通道，一般为 "control" 或 "image"
     * @param message 详细错误信息
     */
    void onTransportError(String channel, String message);
}
