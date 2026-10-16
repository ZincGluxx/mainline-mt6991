# RMX6688 (realme GT7 / Dimensity 9400+ / MT6991) mainline bring-up

主仓库 = linux-7.2.3 内核树本身(git 根 = 本目录)。构建、打包全部在本机(手机
aarch64)完成,无交叉工具链。历史/约束见文末。

## 功能现状

### 已实现 / 正常工作 ✅

| 子系统 | 说明 |
|---|---|
| CPU 频率 | `cpufreq-hw`(performance-domain):A720 339–2400 / X4 622–3300 / X925 798–3730 MHz |
| UFS 存储 | 挂载 Ubuntu rootfs 正常启动 |
| DRM + 面板 | EXDMA3+BLENDER+OUTPROC 新架构;1280×2800;**60/90/120/144Hz**;DSC 1.2;DPMS off/on;`mediatekdrmfb` fbdev 控制台 |
| 显示颜色 | 强制 OVL 只 advertise 8-bit,修 24.04 mutter 选 10-bit 导致的颜色错乱 |
| 屏幕亮度 | AE031 面板 DCS `0x51`(非 PWM),自带 `backlight` 设备;保守上限 `0x800`/默认 `0x600`(厂商正常上限 `0xe0e`/硬件 `0xffe`),GNOME 滑块可调 |
| GPU | panthor / Immortalis-G925:probe、渲染、devfreq/DVFS(338–1612MHz);修了空闲 runtime-PM 循环崩溃 |
| 触摸屏 | Synaptics S3910 (TCM v1, SPI6):单点/多点、量程 12799×27999→1280×2800、边缘校准;固件不自动刷写。AVDD(MT6373 VTP 3.1V)已交给内核:`mt6991-pmic-early` 在 subsys_initcall 用 DT 序列(bare SWINF)拉起,早于 SPI6/syna-tcm 首次 probe,init 不再上电、不再 unbind/rebind |
| USB-C Gadget | CDC-ACM (g_serial),Windows 识别为 USB 串口。**反复拔插都能重新枚举**(完整移植厂商 eUSB2 repeater phy 链——MT6379 repeater 作 phy provider + xsphy 逐次传播 + mtu3 传播 D+ 上拉);重插已移进内核(`mtu3_usb_reconnect()` + `mt6379_charger` VBUS 上升沿),init 不再碰 USB 寄存器 |
| WiFi | MT6653 (connv3, PCIe HIF):扫描/认证/关联/WPA2 四次握手/联网全部正常;conninfra+connadp+connfem+gen4m+PCIe 全量内建。**日志降噪**:`.is_en_sw_emi_read=FALSE` 关掉那条永不回 MSI 的 CB-DMA debug 读(`halMbuRead` 1698→0、EMI MPU 告警 18→0),`kalIsNoMmioReadReason` 不再成立 ⇒ debug dump 自动回退 MMIO,信息不丢 |
| Connectivity 电源栈 | connv3/conninfra、connfem(FEM)、NVRAM 加载、自动上电 |
| 电池电量计 | TI bq27541 @ i2c0 0x55;`/sys/class/power_supply/` 暴露容量/电压/电流/温度,GNOME 显示电量。只读。**i2c0 必须 100kHz**(400kHz 会超时读不到) |
| 充电 (5V 标准) | MT6379(SPMI USID 0x0e):内核驱动(SPMI 控制器 + `mt6379_charger`)把 `AICR`/`ICHG` 提到 3A 并每 3s 轮询重设(抗拔插)。实测 `Charging` ~2.6A;**厂商快充(VOOC)未做** |
| PMIC 栈 | mt6363/mt6373/mt6368/mt6369 四颗全部 `pmic_mng_register_device ok`;connv3 PMIC(mt6373)注册成功;MT6379 eUSB2 repeater probe+power on。主线无 MTK PMIC MFD ⇒ 触摸 VTP 已由内核 `mt6991-pmic-early`(DT 序列 + bare SWINF,subsys_initcall)接管;MD vbuck4/5/6 仍走 init `md_rails_up()` 裸 SPMI(带回读 guard);MT6373 PIO1/2/VAUD18 + GPIO 复位(codec-en GPIO9)由 pmic-early 早期序列先行,init 侧保留 |
| 按键 (电源/音量) | 电源=MT6363 PWRKEY、音量上=MT6363 HOMEKEY(`mt6363-spmi-keys` 轮询 `TOPSTATUS`,因 MT6991 SPMI 无 IRQ domain);音量下=`gpio-keys` GPIO11。实机可用 |
| SCP (RV33 tinysys) | 以模块形式跑:`scp.ko` / `ccci_fsm_scp.ko`(AP 侧 CCISM)/ `connscp.ko` / `clk-mt6991-drv.ko`。`SCP READY` + `freq=190`(DVFS 工作)、ULPOSC cali 通过、AP↔SCP IPI/CCISM 双向通;`scp_ap_ready` sysfs + init `wait_scp_ready()` 保证 modem 在 SCP 完全就绪后再启动 |
| 基带 / Modem (M80) | CCCI/eccci 全量内建 + port_tty.c 给出**真 AT tty `/dev/ttyCCCI0`**;ch14 FS 由内核 `port_fs_reply()` 全量应答(§CCCI_FS_RE_HANDOFF),RPC `0x400F`/`0x4010`/`0x4014` 已内核实现;mdota(901 文件 8.6MB)以 `port_char_mdata` 编进内核。实测 `md_state 0→2→3→4 READY`(25–28 s),长时间零 EE / 零 `-1001`;ModemManager(`mtk-soc` 插件)控制面已通 |
| 热管理 (LVTS) | 31 传感器(25 有效);LVTS 硬件热复位保护,不做软件降频 |
| 桌面系统 | Ubuntu 24.04 + GNOME/mutter 稳定运行 |
| 日志通道 | pstore/ramoops 可保存 panic 前日志;CCCI 日志分三档,`echo 1 > /sys/kernel/ccci/mdsys/debug` 可临时放开 `[ccci_rx]` 等降噪项 |

### 未完成 / 阻塞 ❌

| 子系统 | 状态 / 卡点 |
|---|---|
| 蓝牙 (MT6653) | 正在从厂商 `bt/linux_v2` 改走**主线 BlueZ(`btmtk`/`btmtkuart`, serdev+H4+STP)**:驱动绑定、`hci0` 注册、connv3 电源、MT6653 握手代码均已实现;但 BT UART 经 **UARTHUB**,AP 第一帧始终无回应(`query uart failed -110`)。引脚复用/复位/uarthub 放行序列厂商 pre_on 顺序均已试过,疑点转向 uarthub 数据面与 connv3 上电竞争 |
| 传感器 | 缺厂商总线/地址/上电/初始化固件信息(BMI2xy/AK09918/TCS3720)。厂商架构依赖 SCP sensorhub(SCP 现在已跑起来、`scp.img` 里确含 `bmi2xy`/`akm09919`/`alsps`,该路线可重开);当前 AP 侧直驱四次探针全失败(`bmi270_i2c` chip id 0x3 / ENXIO) |
| 数据面(上网) | ModemManager 控制面(握手/注册/APN profile/`mobile_data`)已通,modem 也已注册在 5G NR(`+C5GREG: 0,1`;`AT+CGDCONT?` 裸 `OK` = AP 从未配过 APN);但 `DATA_ACT` 被 modem 本地拒(`D2CPM_INVAL_ARG/NOT_PERM/NW_UNKNOWN`),`AT+CGACT=1,0` 报同一个 cause ⇒ 门槛在 modem 侧状态、与 MIPC payload 无关。缺 RIL 等价物给 ccmni 配地址 |
| 系统 suspend/resume | 电源域下电会丢 bootloader 状态(MIPI TX PHY、blender 链、SMMU、唤醒源均未自包含) |
| 快充 VOOC/SuperVOOC | 需电荷泵 `mt6379_2p`(i2c3 0x53)+专有协议+OPPO 框架;当前仅 5V 标准充电 |
| GPS | `gps/` 已编为 `gps_drv_dl_v062.ko` 模块并进 initramfs,connv3→connscp→SCP 链路可用(`mt6685 ok`、GDL 控制线程起);缺厂商 userspace(MNLD)与 DTS/EMI 节点 |
| 冷启动无 LK 显示 | 依赖 bootloader 显示的中间级(PHY/mutex/blender)尚未由主线完整接管 |
| 10-bit / HDR scanout | 已试,不可行:厂商 OVL 只支持 ABGR/XBGR2101010,GNOME/mutter 只用 XRGB2101010(厂商未实现),且下游是 bootloader 的 8-bit |
| MT6373/MT6363 PMIC 驱动 | 卡在主线 PMIF 的 EXT_REG_LONG(16-bit 长地址)超时 ⇒ 完整 regulator 驱动未启用;触摸 AVDD 由内核 `mt6991-pmic-early`(DT 序列,bare SWINF)接管,其它上电仍走 raw SPMI/寄存器 |
| 音频 / 扬声器 | AFE/音频时钟/电源域、NXP `tfa98xx` + `mtk-sp-spk-amp`、`tfa98xx.cnt` 均已就绪;**卡在扬声器功放 TFA9873 的供电轨**(i2c6 0x34/0x35 复位已做但仍 NAK);该轨软件不可见,需原理图/实测 |

## 布局

| 路径 | 内容 |
|---|---|
| `arch/arm64/boot/dts/mediatek/mt6991*.dts*` | 板级/SoC DTS + 保留内存(提交进内核树) |
| `arch/arm64/kernel/embedded-dtb.S` 等 | CONFIG_ARM64_EMBEDDED_DTB:把 DTB 用 .incbin 链进 Image |
| `initramfs/` | **独立嵌套 git 仓库**:initramfs 源码(init.c 调试 init / init-rootfs.c rootfs 切换 init)、busybox、`ccbgrab.c`(CCB/DHL 探针,build.sh [1/6] 静态编成 `initramfs/ccbgrab`)、以及 `firmware/`(触摸 + mali CSF + MT6653 WiFi/BT 固件/NVRAM + conninfra/connfem cfg + tfa98xx.cnt + md1drdi.bin)——固件与工具源码都必须在嵌套仓库里,否则本仓库构建的镜像会缺固件/缺 ccbgrab。`init` 与 `cpio_list`、`ccbgrab`(二进制)是生成物,已被 .gitignore |
| `out/` | kbuild O=out 全部产物(.config/Image/System.map/dtb,已 gitignore) |
| `bt/` | pack_boot.sh 工作区:magiskboot + 原厂 boot.img + 成品(已 gitignore) |
| `build.sh` | 一次构建(6 步,见下) |
| `pack_boot.sh` | 把 out 的 Image 打包成可刷 boot.img(magiskboot 往返校验) |
| `rmx6688.fragment` | defconfig 之上合并的 Kconfig 增量 |
| `gen_resmem.py` / `verify_dtb.py` / `fdt` | resmem dtsi 的生成/核对工具 + 原厂 FDT |
| 仓库根 `*.ko` | 模块构建的缓存位置(build.sh 从树内 find 后 cp 过来再嵌进 initramfs);`.ko` 不入库 |

## 一次构建

```sh
# 1) init 必须前台编译过(后台链会吞错误)。默认编 rootfs 切换 init:
gcc -static -O2 -Wall -Wextra -o initramfs/init initramfs/init-rootfs.c
#    调试 init(gcc 换成 init.c),或直接 INIT_SRC=init.c ./build.sh
# 2) 后台构建:
nohup ./build.sh > out/build.log 2>&1 &   # 产物:out/arch/arm64/boot/Image
```

build.sh 步骤:`[1/6]` 静态 init + ccbgrab(INIT_SRC,默认 init-rootfs.c)→
`[2/6]` gen_cpio_list + merge_config + 18 项 CONFIG 校验 → `[3/6]` 只编本板 DTB
→ `[4/6]` 5 个可加载模块(scp / ccci_fsm_scp / connscp / clk-mt6991-drv /
gps_drv_dl_v062)→ `[5/6]` 重新 gen_cpio_list → `[6/6]` Image + **内嵌 DTB
字节级比对**(System.map 符号定位 dd 出来与产物 dtb cmp)。

`README.md` 只存在于本仓库,不在工作区外层;功能现状的更新直接在此文件里做。

## 打包刷机

```sh
./pack_boot.sh        # 自动取下一轮号(N=max+1);或 ./pack_boot.sh 39
```

magiskboot 流程:unpack 原厂 boot.img → 换 kernel → repack → 再 unpack 成品
与 Image 逐字节 cmp。成品 `bt/new-boot.img.N`。

## resmem 工具(只在改保留内存时用)

- `./gen_resmem.py` — 从 `fdt`(原厂 FDT 提取件)重新生成
  `.../mt6991-realme-rmx6688-resmem.dtsi`(期望零 diff;pstore 节点手写在
  板级 dts,SKIP 表跳过)。fdt.dts 可由 `scripts/dtc/dtc -I dtb -O dts fdt` 还原,不入库。
- `./verify_dtb.py` — 构建后核对产物 DTB(out 下)的保留内存与原厂一致。

## Git 历史与纪律

- 历史:import 原版 7.2.3(提交 1)→ 内核 bring-up delta(提交 2)→ 本构建设施
  (提交 3)。今后每次改动按提交记录。
- `initramfs/` 独立仓库:在其目录内单独 commit,不并入内核提交。
- 约束(不得违反):
  - 打包只用 `bt/magiskboot`;别处任何 bootimg.py 都是废件,禁用。
  - 永不修改内核 ext4 相关源码(用户态 init 解决)。
  - init 任何改动先前台 gcc 编译过再后台跑 build.sh。
  - 日志采集 = 硬重启 ring capture(pstore_report + 40s panic → 重启后
    Android 侧读 console-ramoops-0);sdc5 pstore-stash 方案已废弃,不复用。
  - 内核 ext4/initramfs 的 CONFIG 基线见 rmx6688.fragment,build.sh 里 17 项
    校验防回归。
