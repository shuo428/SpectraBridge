# Mock FPGA Test Server

这个目录用于在本机模拟 FPGA，方便通过 Java 前端按钮联调 JNI、native TCP、完整性校验、重排、HDR双平面拆分、质量分析和校准流程。

项目根目录下的 `spectra_bridge_test.exe` 是当前推荐使用的轻量 Mock FPGA 服务端。它不会绕过 Java/DLL 接收链路，而是像真实 FPGA 一样开启控制 TCP 和图像 TCP，收到 `TRIGGER_ONCE` 后发送图像协议头和 RAW16 payload。

## 测试样本目录

测试样本统一生成在：

```text
tools/mock-fpga/test-fixtures
```

生成命令：

```powershell
python tools\mock-fpga\generate_test_fixtures.py
```

该脚本会重建 `test-fixtures` 目录，并同时生成：

- `*.pgm`：C++ 测试程序实际读取的 800×600 8-bit 灰度图
- `*.png`：给人眼预览用
- `manifest.json`：记录样本用途、预期质量状态和路径

C++ 发送时会把 PGM 的 8-bit 灰度值转换成 RAW16_LOW12，也就是每像素 16bit 容器、低 12bit 有效、高 4bit 为 0。

## 当前样本分类

```text
test-fixtures/
  normal/
    normal_pass.*
    normal_warning_hot_pixels.*
    normal_fail_saturation.*

  hdr/
    hdr_pass_hg.* / hdr_pass_lg.*
    hdr_warning_hg.* / hdr_warning_lg.*
    hdr_fail_hg.* / hdr_fail_lg.*

  dark/
    dark_01.* ... dark_08.*

  flat/
    flat_01.* ... flat_08.*

  hdr_dark/
    hdr_dark_01_hg.* / hdr_dark_01_lg.*
    ...

  hdr_flat/
    hdr_flat_01_hg.* / hdr_flat_01_lg.*
    ...
```

## 启动不同测试模式

普通图像采集，轮询 PASS / WARNING / FAIL：

```powershell
.\spectra_bridge_test.exe --scene normal
```

HDR图像采集，轮询 HDR PASS / WARNING / FAIL，payload 为 HG完整平面 + LG完整平面：

```powershell
.\spectra_bridge_test.exe --scene hdr
```

普通暗场校准采集，轮询 8 张 DARK 样本：

```powershell
.\spectra_bridge_test.exe --scene dark
```

普通平场校准采集，轮询 8 张 FLAT 样本：

```powershell
.\spectra_bridge_test.exe --scene flat
```

HDR暗场校准采集，轮询 8 组 HG_DARK / LG_DARK 样本：

```powershell
.\spectra_bridge_test.exe --scene hdr-dark
```

HDR平场校准采集，轮询 8 组 HG_FLAT / LG_FLAT 样本：

```powershell
.\spectra_bridge_test.exe --scene hdr-flat
```

如果需要指定端口：

```powershell
.\spectra_bridge_test.exe --control-port 5000 --image-port 5001 --scene hdr
```

## 与前端模块的对应关系

- 普通图像采集：启动 `--scene normal`
- HDR图像采集：启动 `--scene hdr`
- 普通校准与缺陷地图 / 暗场：启动 `--scene dark`
- 普通校准与缺陷地图 / 平场：启动 `--scene flat`
- HDR校准与缺陷地图 / HDR暗场：启动 `--scene hdr-dark`
- HDR校准与缺陷地图 / HDR平场：启动 `--scene hdr-flat`

注意：当前 FPGA 控制协议里没有“场景类型”字段，所以测试程序无法从 Java 的 `captureScene` 自动知道你正在采普通图、HDR图、暗场还是平场。测试时需要你启动对应 `--scene`。

## 当前协议

- 控制通道默认：`127.0.0.1:5000`
- 图像通道默认：`127.0.0.1:5001`
- 图像尺寸：`800 x 600`
- 像素格式：`RAW16_LOW12`
- payload 读出顺序：`GLUX1605_HDR_4LANE_INTERLEAVED_EFFECTIVE`
- HDR payload 约定：先 HG 完整平面，再 LG 完整平面

## 自检

普通单平面自检：

```powershell
.\spectra_bridge_test.exe --scene normal --self-test
```

HDR双平面自检：

```powershell
.\spectra_bridge_test.exe --scene hdr --self-test
```

HDR暗场/平场也可以自检：

```powershell
.\spectra_bridge_test.exe --scene hdr-dark --self-test
.\spectra_bridge_test.exe --scene hdr-flat --self-test
```

自检会验证：

- 样本能被加载
- 图像协议头能通过 `ParseImageFrameHeader`
- 普通单平面 payload 长度为 `960000 bytes`
- HDR双平面 payload 长度为 `1920000 bytes`
- GLUX1605 4-lane 重排后能恢复到正常行列顺序
- HDR模式下 HG/LG 两个平面都能被正确拆分和重排
