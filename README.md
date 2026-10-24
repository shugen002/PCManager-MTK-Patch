# MTK-Patch

让装了 **MediaTek MT7922** 网卡的电脑，在用华为 PC Manager 时能正常广播蓝牙 ——
不然「多屏协同 / 华为分享」根本扫不到这台电脑。

补丁就是 PC Manager 目录下的一个 `winmm.dll`：它照常转发所有系统调用，只在检测到
这颗网卡时，把华为本来要发的广播改从 MT7922 自己的蓝牙通道发出去。

## 安装

1. 打开 [Releases](../../releases)，下载最新版的 `winmm.dll`。
2. 找到 PC Manager 的安装目录（通常是 `C:\Program Files\Huawei\PCManager`，
   确认里面有 `HiConnectivityService.exe`）。
3. 用**管理员** PowerShell 执行：

```powershell
Stop-Service HiConnectivityService -Force
Copy-Item .\winmm.dll "C:\Program Files\Huawei\PCManager\" -Force
Start-Service HiConnectivityService
```

4. 重新打开 PC Manager 的多屏协同，手机上应该就能看到这台电脑了。

## 卸载

```powershell
Stop-Service HiConnectivityService -Force
Remove-Item "C:\Program Files\Huawei\PCManager\winmm.dll"
Start-Service HiConnectivityService
```

## 注意

- 只对 MT7922 生效。没有这颗网卡（`\\.\MTKBTFilter` 设备不存在）时它就是个普通
  的 winmm 转发层，什么都不会改。
- 日志：`C:\ProgramData\MTK-Patch\mt7922_hci.log`
- PC Manager 升级可能把这个文件覆盖掉，重装一次就行。
- 杀毒软件可能对「程序目录下放了个同名系统 DLL」报警，放行即可。

## 从源码构建

需要 VS 2022/2026（含 MASM）、Windows SDK、CMake ≥ 3.20、Python 3：

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

推一个 `v*` 标签，GitHub Actions 会自动构建、校验导出表并把 `winmm.dll` 发布到 Release。

导出表是构建时从本机 `C:\Windows\System32\winmm.dll` 读出来的，所以要在和目标机器同一代 Windows 上构建。
