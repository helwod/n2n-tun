# n2n-tun

> 语言 / Language: [简体中文](README.zh.md) · [English](README.en.md)

**n2n-tun** 是基于 [ntop/n2n](https://github.com/ntop/n2n) 的分支，增加了对 **Windows TUN / TAP** 虚拟网卡的支持。

**n2n-tun** is a fork of [ntop/n2n](https://github.com/ntop/n2n) that adds support for **TUN / TAP** virtual adapters on Windows.

## 关键说明 / Key notes

- 使用 **TUN** 网卡需要 `wintun.dll`（[WinTun](https://www.wintun.net/)），请放到 `edge.exe` 同目录（建议 0.14.1 版本）。
  Using **TUN** requires `wintun.dll` ([WinTun](https://www.wintun.net/)) placed next to `edge.exe` (v0.14.1 recommended).
- 使用 **TAP** 网卡需要 TAP-Windows 驱动（[下载](https://build.openvpn.net/downloads/releases/)），建议 9.24.7 版本。
  Using **TAP** requires the TAP-Windows driver ([download](https://build.openvpn.net/downloads/releases/)), v9.24.7 recommended.
- 新增参数：`-w` 优先 TUN 网卡；`-d <名称>` 设置 / 自动创建网卡名称。
  New options: `-w` to prefer TUN over TAP-Win32; `-d <name>` to set / auto-create the adapter name.

## 下载 / Download

Windows x64 预编译包（含 `edge.exe`、`supernode.exe`、`wintun.dll` 三个文件）：

Windows x64 prebuilt package (contains `edge.exe`, `supernode.exe`, `wintun.dll`):

- GitHub Releases: https://github.com/helwod/n2n-tun/releases

## 完整文档 / Full documentation

- [简体中文文档 / Chinese](README.zh.md)
- [English documentation](README.en.md)
