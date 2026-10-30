# n2n-tun（支持 Windows TUN/TAP 网卡的 n2n 分支）
# n2n-tun (n2n fork with Windows TUN/TAP support)

[![Build Status](https://travis-ci.org/ntop/n2n.png?branch=dev)](https://travis-ci.org/ntop/n2n)

本仓库（helwod/n2n-tun）基于 ntop/n2n，增加了对 Windows 平台 **TUN / TAP** 虚拟网卡的支持。
This repository (helwod/n2n-tun) is based on ntop/n2n and adds support for **TUN / TAP** virtual network adapters on Windows.

- 使用 TUN 网卡需要 `wintun.dll`（[WinTun 官网](https://www.wintun.net/)），请将该 dll 放到 `edge.exe` 同目录下（建议使用 0.14.1 版本）。
  Using the TUN adapter requires `wintun.dll` ([WinTun](https://www.wintun.net/)). Place the dll in the same directory as `edge.exe` (version 0.14.1 recommended).
- 使用 TAP 网卡需要 TAP-Windows 驱动（[Tap-Windows 下载](https://build.openvpn.net/downloads/releases/)），建议使用 9.24.7 版本。
  Using the TAP adapter requires the TAP-Windows driver ([Tap-Windows](https://build.openvpn.net/downloads/releases/)), version 9.24.7 recommended.
- 新增参数：`-w` 使用 TUN 网卡（默认优先 TAP-Win32）；`-d` 设置网卡名称，若名称不存在会自动创建。
  New options: `-w` to prefer the TUN adapter over TAP-Win32; `-d <name>` to set the adapter name (auto-created if it does not exist).

---

# n2n

n2n is a light VPN software which makes it easy to create virtual networks bypassing intermediate firewalls.
n2n 是一款轻量级 VPN 软件，可轻松创建虚拟网络并穿透中间防火墙。

In order to start using n2n, two elements are required:
使用 n2n 需要两个组件：

- A _supernode_: it allows edge nodes to announce and discover other nodes. It must have a port publicly accessible on internet.
  一个 _supernode_（超级节点）：用于让边缘节点宣告并发现彼此，必须在公网上开放一个端口。
- _edge_ nodes: the nodes which will be a part of the virtual networks.
  多个 _edge_（边缘节点）：构成虚拟网络的实际节点。

A virtual network shared between multiple edge nodes in n2n is called a _community_. A single supernode can relay multiple communities and a single computer can be part of multiple communities at the same time. An encryption key can be used by the edge nodes to encrypt the packets within their community.
多个 edge 节点共享的虚拟网络称为一个 _community_（社区）。一个 supernode 可中转多个社区，一台机器也可同时加入多个社区。edge 节点可使用加密密钥对社区内流量加密。

n2n tries to establish a direct peer-to-peer connection via udp between the edge nodes when possible. When this is not possible (usually due to special NAT devices), the supernode is also used to relay the packets.
n2n 会尽量在 edge 之间建立 UDP 直连（P2P）；当不可行时（通常是特殊 NAT 设备），supernode 也会负责中转。

## Quick Setup / 快速开始

Some Linux distributions already provide n2n as a package so a simple `sudo apt install n2n` will do the work. Alternatively, up-to-date packages for most distributions are available on [ntop repositories](http://packages.ntop.org/).
部分 Linux 发行版已自带 n2n，直接 `sudo apt install n2n` 即可；也可通过 [ntop 仓库](http://packages.ntop.org/) 安装最新包。

On host1 run / 在主机 1 执行：

```sh
$ sudo edge -c mynetwork -k mysecretpass -a 192.168.100.1 -f -l supernode.ntop.org:7777
```

On host2 run / 在主机 2 执行：

```sh
$ sudo edge -c mynetwork -k mysecretpass -a 192.168.100.2 -f -l supernode.ntop.org:7777
```

Now the two hosts can ping each other.
此时两台主机即可互相 ping 通。

**IMPORTANT** It is strongly advised to choose a custom community name (`-c`) and a secret encryption key (`-k`) in order to prevent other users from connecting to your computer. For the privacy of your data sent and to reduce the server load of `supernode.ntop.org`, it is also suggested to set up a custom supernode as explained below.
**重要** 强烈建议自定义社区名（`-c`）和加密密钥（`-k`），避免他人接入你的计算机。同时为了保护数据隐私并降低 `supernode.ntop.org` 的负载，建议自建 supernode（见下文）。

## Setting up a Custom Supernode / 自建超级节点

You can create your own infrastructure by setting up a supernode on a public server (e.g. a VPS). You just need to open a single port (1234 in the example below) on your firewall (usually `iptables`).
你可以在一台公网服务器（如 VPS）上自建 supernode，只需在防火墙（通常是 `iptables`）开放一个端口（下例为 1234）。

1. Install the n2n package / 安装 n2n 包
2. Edit `/etc/n2n/supernode.conf` and add the following / 编辑该文件并加入：
   ```
   -p=1234
   ```
3. Start the supernode service with `sudo systemctl start supernode` / 启动服务：`sudo systemctl start supernode`
4. Optionally enable supernode start on boot: `sudo systemctl enable supernode` / 可选，设置开机自启：`sudo systemctl enable supernode`

Now the supernode service should be up and running on port 1234. On your edge nodes you can now specify `-l your_supernode_ip:1234` to use it. All the edge nodes must use the same supernode.
此时 supernode 已在 1234 端口运行。edge 节点使用 `-l 你的supernode_ip:1234` 接入，所有 edge 必须使用同一个 supernode。

## Manual Compilation / 手动编译

On Linux, compilation from source is straight forward:
在 Linux 上从源码编译很简单：

```sh
./autogen.sh
./configure
make

# optionally install / 可选安装
make install
```

For Windows, MacOS, optimizations and general building options, please check out [Building documentation](doc/Building.md) for compilation and running.
关于 Windows、macOS、编译优化等通用构建选项，请参阅 [构建文档](doc/Building.md)。

**Windows Note / Windows 说明:** n2n supports WireGuard's wintun driver for better performance. Use `-w` option to prefer wintun over TAP-Win32. On this fork, place `wintun.dll` next to `edge.exe` and use `-w` for TUN, or rely on TAP-Windows for TAP. Use `-d <name>` to set / auto-create the adapter name.
**Windows 说明：** n2n 支持 WireGuard 的 wintun 驱动以获得更好性能。本分支使用 `-w` 优先 TUN 网卡；将 `wintun.dll` 放在 `edge.exe` 同目录，用 `-w` 启用 TUN，或依赖 TAP-Windows 使用 TAP。用 `-d <名称>` 设置或自动创建网卡名称。

**IMPORTANT** It is generally recommended to use the [latest stable release](https://github.com/ntop/n2n/releases). Please note that the current _dev_ branch usually is not guaranteed to be backward compatible neither with the latest stable release nor with previous _dev_ states. On the other hand, if you dare to try bleeding edge features, you are encouraged to compile from _dev_ – just keep track of sometimes rapidly occuring changes. Feedback in the _Issues_ section is appreciated.
**重要** 建议使用[最新稳定版](https://github.com/ntop/n2n/releases)。当前 _dev_ 分支不保证与最新稳定版或历史 _dev_ 状态向后兼容。若你愿意尝鲜，欢迎从 _dev_ 编译，但请注意其变化可能较快，并欢迎在 Issues 中反馈。

## Security Considerations / 安全说明

When payload encryption is enabled (provide a key using `-k`), the supernode will not be able to decrypt the traffic exchanged between two edge nodes but it will know that edge A is talking with edge B.
启用负载加密（用 `-k` 提供密钥）后，supernode 无法解密两个 edge 之间的流量，但能知道 edge A 与 edge B 在通信。

The choice of encryption schemes that can be applied to payload has recently been enhanced. Please have a look at [Crypto description](doc/Crypto.md) for a quick comparison chart to help make a choice. n2n edge nodes use AES encryption by default. Other ciphers can be chosen using the `-A_` option.
可应用于负载的加密方案已大幅增强，详见[加密说明](doc/Crypto.md)。n2n edge 默认使用 AES 加密，也可用 `-A_` 选择其他算法。

A benchmark of the encryption methods is available when compiled from source with `tools/n2n-benchmark`.
加密方法的基准测试可在源码编译后通过 `tools/n2n-benchmark` 运行。

The header which contains some metadata like the virtual MAC address of the edge nodes, their IP address, their real hostname and the community name optionally can be encrypted applying `-H` on the edges.
包含虚拟 MAC、IP、真实主机名、社区名等元数据的头部可用 edge 上的 `-H` 选项进行可选加密。

## Advanced Configuration / 高级配置

More information about communities, support for multiple supernodes, routing, traffic restrictions and on how to run an edge as a service is available in the [more detailed documentation](doc/Advanced.md).
关于社区、多 supernode、路由、流量限制以及将 edge 作为服务运行的更多信息，请参阅[详细文档](doc/Advanced.md)。

## Contribution / 贡献

You can contribute to n2n in various ways:
你可以通过多种方式参与贡献：

- Update an [open issue](https://github.com/ntop/n2n/issues) or create a new one with detailed information / 更新或新建 [Issue](https://github.com/ntop/n2n/issues) 并提供详细信息
- Propose new features / 提出新功能建议
- Improve the documentation / 完善文档
- Provide pull requests with enhancements / 提交增强型 Pull Request

For details about the internals of n2n check out the [Hacking guide](https://github.com/ntop/n2n/blob/dev/doc/Hacking.md).
关于 n2n 内部实现细节，请参阅 [Hacking 指南](https://github.com/ntop/n2n/blob/dev/doc/Hacking.md)。

## Further Readings and Related Projects / 延伸阅读与相关项目

Answers to frequently asked questions can be found in our [FAQ document](https://github.com/ntop/n2n/blob/dev/doc/Faq.md).
常见问题请见 [FAQ 文档](https://github.com/ntop/n2n/blob/dev/doc/Faq.md)。

Here is a list of third-party projects connected to this repository:
以下是与本仓库相关的第三方项目：

- Collection of pre-built binaries for Windows: [lucktu](https://github.com/lucktu/n2n)
- n2n for Android: [hin2n](https://github.com/switch-iot/hin2n)
- Docker images: [Docker Hub](https://hub.docker.com/r/supermock/supernode/)
- Go bindings, management daemons and CLIs for n2n edges and supernodes, Docker, Kubernetes & Helm Charts: [pojntfx/gon2n](https://pojntfx.github.io/gon2n/)
- Windows GUI (along with a custom version of n2n) but also working with regular n2n: [HappyNet](https://github.com/happynclient/happynwindows)

---

(C) 2007-22 - ntop.org and contributors
