# 按窗口尺寸采集

[English](README.md) · [下载](https://github.com/nonenty/obs-window-size-capture/releases/latest)

适用于 Windows OBS Studio，按**可执行文件名和窗口尺寸**匹配目标，不受标题变化影响。支持合成同一进程的浮动窗口，以及采集进程音频。

已验证 **OBS 32.0.4、33.0.1 / Windows x64**。视频输出为 30 fps / SDR。窗口采集需要 Windows 10 1903 或更新版本；进程音频需要 Windows Build 20348 或更新版本，同时须满足 OBS 自身的系统要求。

## 安装

1. 从 [Releases](https://github.com/nonenty/obs-window-size-capture/releases/latest) 下载 `window-size-capture-<版本号>-windows-x64.zip`，退出 OBS。
2. 打开 OBS Studio 的安装目录，即包含 `bin`、`data` 和 `obs-plugins` 的文件夹。
3. 将 ZIP 中的内容解压到该目录，合并文件夹并覆盖已有插件文件。
4. 启动 OBS，添加「按窗口尺寸采集」来源。

ZIP 内的目录与 OBS 安装目录一致：

```text
bin/64bit/window-size-capture-worker.exe
obs-plugins/64bit/window-size-capture.dll
data/obs-plugins/window-size-capture/
```

卸载时退出 OBS，删除上述两个文件及插件数据目录即可。

## 使用

在来源属性中选择窗口，保存其可执行文件名和尺寸；窗口未出现时刷新列表。默认仅采集客户区，如需包含标题栏，关闭该选项后重新选择窗口。尺寸容差默认为 4 像素。

「采集主窗口及同进程窗口」会按相对位置合成浮动窗口，超出主窗口的部分被裁剪。「采集音频」会包含目标进程及其子进程，无法按窗口区分音频。同一程序的同尺寸窗口无法可靠区分。

## 构建

使用 WSL 和 MinGW-w64，Windows 上需已安装 OBS：

```bash
sudo apt-get install git python3 mingw-w64
export OBS_BIN='/mnt/c/Program Files/obs-studio/bin/64bit'
./package.sh
./test.sh  # 需要可交互的 Windows 桌面和显卡
```

构建时下载 OBS 32.0.4 头文件。可通过 `OBS_SDK`、`BUILD_DIR` 指定 SDK 和构建目录。两个发布 ZIP 位于 `build/`，`build/release/` 为可直接复制到 OBS 的文件。GitHub Actions 检查编译和 ZIP 内容，本地测试检查采集像素、生命周期、音频和辅助进程恢复。

[版本记录](CHANGELOG.md) · [GPL-2.0-or-later](LICENSE) · [第三方许可](THIRD_PARTY_NOTICES.md)
