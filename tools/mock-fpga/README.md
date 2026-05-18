# Mock FPGA Test Server

这个目录下的程序用于在本机模拟 FPGA，方便你直接通过 Java 前端按钮联调 JNI 和 native TCP 逻辑。

## 作用

- 开两条 TCP 监听
  - 控制通道：`5000`
  - 图像通道：`5001`
- 支持这些控制流程
  - `sendReset()`
  - `sendTriggerOnce()`
  - `sendQueryStatus()`
  - `sendFullConfig(byte[512])`
- 当收到 `TRIGGER_ONCE` 后，向图像 TCP 发送一帧模拟图像

## 当前协议

- 所有多字节整数都按小端发送
- 图像头使用当前 native 侧的 `ImageFrameHeader`
- 图像为 `800 x 600`
- 每像素 `16bit`
- 只有低 `12bit` 有效

## 编译

如果你本机使用的是 MinGW，可以直接在这个项目根目录运行：

```powershell
g++ -std=c++17 tools\mock-fpga\MockFpgaServer.cpp -lws2_32 -o tools\mock-fpga\MockFpgaServer.exe
```

## 运行顺序

1. 先启动 `MockFpgaServer.exe`
2. 再启动你的 Java 程序
3. 让 Java/JNI 客户端连接：
   - 控制 TCP：`127.0.0.1:5000`
   - 图像 TCP：`127.0.0.1:5001`
4. 在前端依次测试按钮：
   - 查询状态
   - 发送配置
   - 触发单帧

## 预期回调

- 点击“查询状态”后：收到 `onStatus(...)`
- 点击“发送配置”后：收到 `onConfigAck(...)`
- 点击“单次触发”后：
  - 先可能收到一次 `onStatus(...)`
  - 然后收到 `onImageFrame(...)`
