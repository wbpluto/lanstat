# lanstat

**[English](README.md) | [简体中文](README_ZH.md)**

专为 OpenWrt / ImmortalWrt 路由器打造的轻量级纯 C11 实时局域网带宽监控工具，单二进制无外部运行时依赖。

**每秒看清哪台设备在占用网速**——常驻内存仅约 `1.8 MB`，纯内存统计 **零闪存写入**，适合 7×24 小时长期运行。

![lanstat 实时带宽监控](docs/screenshot-zh.png)

## 核心特性

- **极致轻量化**：单线程原生 C11 实现，无脚本解释器或数据库依赖，零闪存写入不损耗路由器寿命。
- **高效内核抓包**：采用 `AF_PACKET` + `TPACKET_V2` 零拷贝环形缓冲区与 BPF 过滤（支持 IPv4 / IPv6 / VLAN），自动降级兼容 `recv(..., MSG_TRUNC)`。
- **实时可视化面板**：内置 Gzip 压缩 Web 面板（默认 `6670` 端口），支持 1s–5s 实时速率排行、全网与单设备多窗口趋势图（60 秒 / 60 分钟 / 24 小时 / 31 天）、流量占比条与收发包率（`PPS` 及平均包长）。
- **灵活统计与筛选**：支持 **全部 / 仅外网 / 仅内网** 流量口径切换、按 MAC 合并 IPv4/IPv6、仅显示活跃设备、6 态设备/IP/MAC 排序，以及页面内一键热切换监听接口。
- **离线设备与厂商识别**：直接解析 `/tmp/dhcp.leases`、`/etc/hosts`、`/etc/ethers`（不衍生 DNS 子进程），内置 **21,000+ 条离线 IEEE OUI 厂商字典** 与本地设备备注。
- **隐私保护与多语言**：一键 **隐私打码** 方便截图（搜索仍支持明文匹配）；四格滑块式语言切换（**简 / 繁 / EN / РУ**，默认自动匹配浏览器语言）；适配桌面与手机端 Safari / Chrome / Edge。

## 安装方法

前往 [GitHub Releases](../../releases) 根据路由器架构（可通过 `uname -m` 或 `opkg print-architecture` / `apk --print-arch` 查看）下载对应安装包：

### OpenWrt 25.12 及以上（`.apk`）

```sh
apk add --allow-untrusted lanstat-*.apk
/etc/init.d/lanstat enable
/etc/init.d/lanstat start
```

### OpenWrt 23.05 / 24.10（`.ipk`）

```sh
opkg install lanstat_*.ipk
/etc/init.d/lanstat enable
/etc/init.d/lanstat start
```

### 通用静态二进制包（`.tar.gz`）

```sh
mkdir -p /tmp/lanstat-pkg
tar -xzf lanstat-<arch>.tar.gz -C /tmp/lanstat-pkg
sh /tmp/lanstat-pkg/install.sh
```

安装完成后在浏览器打开 `http://<路由器IP>:6670/` 即可查看。

## 配置说明（`/etc/config/lanstat`）

```uci
config lanstat 'main'
	option enabled '1'
	option interface 'br-lan'
	option bind '0.0.0.0'
	option port '6670'
	option promisc '1'
	option track_external '0'
	option max_hosts '512'
	option ring_kb '1024'
```

- `interface`：监听的 OpenWrt 逻辑接口（如 `lan`）或设备名（如 `br-lan`）。
- `bind`：Web 监听地址（默认 `0.0.0.0`；如需仅限局域网访问可改为路由器内网 IP，如 `192.168.1.1`）。
- `port`：Web 面板端口（默认 `6670`）。
- `promisc`：是否开启混杂模式（桥接接口建议保持 `1`）。
- `track_external`：是否记录公网外部对端（默认 `0` 仅统计局域网设备）。
- `max_hosts`：预分配终端记录槽位数（默认 `512`）。
- `ring_kb`：`TPACKET_V2` 接收环形缓冲区大小，单位 KiB（默认 `1024`）。

修改配置后执行 `/etc/init.d/lanstat reload` 生效。

## 安全与防火墙提示

- 标准 OpenWrt 防火墙默认拒绝 `wan` 侧入站，因此 `6670` 端口仅局域网可访问。
- 若部署在单臂路由、旁路由或放通入站防火墙的环境中，请将 `option bind` 设为内网 IP（或配置防火墙规则），避免将面板及 `POST /api/iface` 接口暴露到公网。

## 硬件加速 / 流卸载说明

`lanstat` 通过 Linux 内核 `AF_PACKET` 抓包统计：

- **HWNAT / SFE / 软硬件流卸载（Flow Offloading）**：已建立的长连接被卸载至硬件或快路径后将绕过内核网桥，测得的有线持续速率可能显著偏低（通常少计 60%–95%）。
- **MediaTek WED（无线硬件加速）**：若在 MT798x 设备开启 WED，无线卸载流量同样会绕过内核统计；如需 100% 精确统计可临时关闭流卸载/WED。

## 本地构建与多架构测试

```sh
# 检查 JS 语法、运行主机 ASan+UBSan 单测并打包默认 aarch64
make clean all

# 交叉编译验证全部 5 种架构（-Wall -Wextra -Werror）
make check-arches

# 打包指定架构的静态安装包（x86_64 / aarch64 / armv7 / mipsel / mips）
make package ARCH=x86_64
```

## 提交到 OpenWrt / ImmortalWrt 官方源

1. 在 GitHub 推送代码并打正式版本 Tag（如 `v1.0.0`），触发 Actions 自动发布多架构 `.ipk` / `.apk` 与静态包。
2. 将 `openwrt-package/lanstat/Makefile` 中的 `OWNER`、`PKG_MAINTAINER` 与 `PKG_HASH` 替换为真实仓库信息与 Release tarball 的 SHA-256。
3. 将 `openwrt-package/lanstat/` 目录作为 `net/lanstat/` 提 PR 至 `openwrt/packages` 或 `immortalwrt/packages`。

## 数据来源与开源协议

- 项目代码基于 [MIT License](LICENSE) 开源。
- 内置离线厂商字典 `web/oui_packed.txt` 整理自 IEEE Standards Association 公开的 OUI 注册表。
