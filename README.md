# MTK-Patch

利用华为电脑关键会默认回落到Realtek蓝牙网卡处理的特性，通过拦截原来 Realtek 的操作，转到 MT7922 的厂商HCI通道，从而让华为电脑管家能够正常收发 BLE 广播。

## 安装

1. 打开 [Releases](../../releases)，下载最新版的 `winmm.dll`。
2. 找到 PC Manager 的安装目录（通常是 `C:\Program Files\Huawei\PCManager`，
   确认里面有 `HiConnectivityService.exe`）。
3. 将下载的 `winmm.dll` 放到 PC Manager 的安装目录下
4. 在任务管理器中，重启 HiConnectivityService 服务

## 卸载

1. 在任务管理器中，停止 HiConnectivityService 服务
2. 删除 PC Manager 安装目录下的 `winmm.dll`（通常是 `C:\Program Files\Huawei\PCManager\winmm.dll`）
3. 重启 HiConnectivityService 服务

## 注意

- 只对 MT7922 及其马甲型号(如`RZ616`)生效，其他网卡不会受到影响。
- 日志：`C:\ProgramData\MTK-Patch\mt7922_hci.log`
- 只测试了 RZ616 型号，其他马甲型号未验证。
- `华为分享` 功能依赖经典蓝牙的 GATT 服务，暂时不支持。

## 从源码构建

需要 VS 2022/2026（含 MASM）、Windows SDK、CMake ≥ 3.20、Python 3：

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

推一个 `v*` 标签，GitHub Actions 会自动构建、校验导出表并把 `winmm.dll` 发布到 Release。

导出表是构建时从本机 `C:\Windows\System32\winmm.dll` 读出来的，所以要在和目标机器同一代 Windows 上构建。
