> 语言 / Language: [简体中文](README.zh.md) · [English](README.en.md)

# n2n-tun（支持 Windows TUN/TAP 网卡的 n2n 分支）

本仓库（helwod/n2n-tun）基于 [ntop/n2n](https://github.com/ntop/n2n)，增加了对 Windows 平台 **TUN / TAP** 虚拟网卡的支持。

## Windows 专属说明

- 使用 **TUN** 网卡需要 `wintun.dll`（[WinTun 官网](https://www.wintun.net/)）。请将该 dll 放到 `edge.exe` 同目录下（**建议使用 0.14.1 版本**）。
- 使用 **TAP** 网卡需要 TAP-Windows 驱动（[Tap-Windows 下载](https://build.openvpn.net/downloads/releases/)），建议使用 9.24.7 版本。
- 新增参数：
  - `-w`：使用 TUN 网卡（默认优先 TAP-Win32）。
  - `-d <名称>`：设置网卡名称，若名称不存在会自动创建。

## n2n 简介

n2n 是一款轻量级 VPN 软件，可轻松创建虚拟网络并穿透中间防火墙。

使用 n2n 需要两个组件：

- 一个 **supernode（超级节点）**：用于让边缘节点宣告并发现彼此，必须在公网上开放一个端口。
- 多个 **edge（边缘节点）**：构成虚拟网络的实际节点。

多个 edge 节点共享的虚拟网络称为一个 **community（社区）**。一个 supernode 可中转多个社区，一台机器也可同时加入多个社区。edge 节点可使用加密密钥对社区内流量加密。

n2n 会尽量在 edge 之间建立 UDP 直连（P2P）；当不可行时（通常是特殊 NAT 设备），supernode 也会负责中转。

## 快速开始

部分 Linux 发行版已自带 n2n，直接 `sudo apt install n2n` 即可；也可通过 [ntop 仓库](http://packages.ntop.org/) 安装最新包。

在主机 1 执行：

```sh
sudo edge -c mynetwork -k mysecretpass -a 192.168.100.1 -f -l supernode.ntop.org:7777
```

在主机 2 执行：

```sh
sudo edge -c mynetwork -k mysecretpass -a 192.168.100.2 -f -l supernode.ntop.org:7777
```

此时两台主机即可互相 ping 通。

**重要** 强烈建议自定义社区名（`-c`）和加密密钥（`-k`），避免他人接入你的计算机。同时为了保护数据隐私并降低 `supernode.ntop.org` 的负载，建议自建 supernode（见下文）。

## 自建超级节点

你可以在一台公网服务器（如 VPS）上自建 supernode，只需在防火墙（通常是 `iptables`）开放一个端口（下例为 1234）。

1. 安装 n2n 包；
2. 编辑 `/etc/n2n/supernode.conf` 并加入：
   ```
   -p=1234
   ```
3. 启动服务：`sudo systemctl start supernode`；
4. 可选，设置开机自启：`sudo systemctl enable supernode`。

此时 supernode 已在 1234 端口运行。edge 节点使用 `-l 你的supernode_ip:1234` 接入，所有 edge 必须使用同一个 supernode。

## 手动编译

在 Linux 上从源码编译很简单：

```sh
./autogen.sh
./configure
make
# 可选安装
make install
```

关于 Windows、macOS、编译优化等通用构建选项，请参阅 [构建文档](doc/Building.md)。

**Windows 说明：** 本分支使用 `-w` 优先 TUN 网卡；将 `wintun.dll` 放在 `edge.exe` 同目录，用 `-w` 启用 TUN，或依赖 TAP-Windows 使用 TAP。用 `-d <名称>` 设置或自动创建网卡名称。仓库根目录提供了 `do_build.bat` 可一键在 Windows 上编译（需 MSYS2 / MinGW-w64 环境）。

**重要** 建议使用[最新稳定版](https://github.com/ntop/n2n/releases)。当前 _dev_ 分支不保证与最新稳定版或历史 _dev_ 状态向后兼容。若你愿意尝鲜，欢迎从 _dev_ 编译，但请注意其变化可能较快，并欢迎在 Issues 中反馈。

## 安全说明

启用负载加密（用 `-k` 提供密钥）后，supernode 无法解密两个 edge 之间的流量，但能知道 edge A 与 edge B 在通信。

可应用于负载的加密方案已大幅增强，详见[加密说明](doc/Crypto.md)。n2n edge 默认使用 AES 加密，也可用 `-A_` 选择其他算法。

加密方法的基准测试可在源码编译后通过 `tools/n2n-benchmark` 运行。

包含虚拟 MAC、IP、真实主机名、社区名等元数据的头部可用 edge 上的 `-H` 选项进行可选加密。

## 高级配置

关于社区、多 supernode、路由、流量限制以及将 edge 作为服务运行的更多信息，请参阅[详细文档](doc/Advanced.md)。

## 贡献

你可以通过多种方式参与贡献：

- 更新或新建 [Issue](https://github.com/ntop/n2n/issues) 并提供详细信息；
- 提出新功能建议；
- 完善文档；
- 提交增强型 Pull Request。

关于 n2n 内部实现细节，请参阅 [Hacking 指南](https://github.com/ntop/n2n/blob/dev/doc/Hacking.md)。

## 延伸阅读与相关项目

常见问题请见 [FAQ 文档](https://github.com/ntop/n2n/blob/dev/doc/Faq.md)。

以下是与本仓库相关的第三方项目：

- Windows 预编译二进制集合：[lucktu](https://github.com/lucktu/n2n)
- Android 版 n2n：[hin2n](https://github.com/switch-iot/hin2n)
- Docker 镜像：[Docker Hub](https://hub.docker.com/r/supermock/supernode/)
- Go 绑定、管理守护进程与 CLI（支持 Docker、Kubernetes & Helm Charts）：[pojntfx/gon2n](https://pojntfx.github.io/gon2n/)
- Windows GUI：[HappyNet](https://github.com/happynclient/happynwindows)

---

(C) 2007-22 - ntop.org and contributors
