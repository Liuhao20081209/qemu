# vmqemu — Termux 上的 QEMU 虚拟机管理器

单文件 C + ncurses 写的 TUI 虚拟机管理器，把 Termux 下手工敲 qemu 参数这件事变成菜单操作
原版是 1000 多行的 bash 脚本，vmqemu 是 C 重写版，并补上了 QMP 控制（暂停、恢复、关机、快照、挂起）

当前版本 v1.41

## 特性

- 多架构：x86_64、aarch64、riscv64、ppc、s390x
- 一台机器一套配置，存在 `~/start/<虚拟机名>.sh`，可读可改
- 磁盘、ISO、软盘、附加盘、共享目录
- 网卡、用户模式网络、SSH 端口转发、DNS
- 显示：无头、VNC、SPICE、SDL、GTK
- UEFI：ARM 用 edk2，x86 用 OVMF，变量盘按固件大小自动生成
- 串口控制台：把 qemu 的串口挂到 pty，直接在界面里显示和输入，Ctrl+] 退回菜单
- 运行控制：暂停、恢复、ACPI 关机、强制停止、在线快照、挂起到磁盘
- 启动前环境自检：缺模拟器、缺固件会先弹一页说明，不挡着用
- 端口自动避让：SSH 从 2222、VNC 从 5901 起扫，跳过被占和本机其他虚拟机

## 依赖

Termux：

```bash
pkg install clang ncurses-utils ncurses qemu-system-x86-64-headless qemu-system-aarch64-headless qemu-utils
```

Debian / Ubuntu：

```bash
sudo apt install gcc libncurses-dev qemu-system-x86 qemu-utils
```

只编译本程序时，真正必须的是 C 编译器和 ncurses 宽字符库

## 编译

```bash
gcc -O2 -Wall -o vmqemu vmqemu.c -lncursesw
```

Termux 上用 clang 也可以：

```bash
clang -O2 -Wall -o vmqemu vmqemu.c -lncursesw
```

## 快速开始

```bash
./vmqemu
```

第一次启动会先做环境自检，缺东西会列出缺哪项和对应安装命令，按 q 或 Esc 跳过
然后在主页按提示新建虚拟机，先建磁盘再选固件，最后按 b 启动并接控制台

## 界面与按键

### 主页

| 按键 | 作用 |
|---|---|
| ↑ ↓ | 选择虚拟机 |
| Enter | 打开虚拟机页 |
| b | 没在跑就启动，在跑就接串口控制台 |
| c | 打开配置页 |
| x | 删除虚拟机 |
| Esc | 退出 |
| g / G | 跳到第一台 / 最后一台 |

### 虚拟机页

列出启动、控制台、配置、暂停、恢复、关机、强制停止、快照、挂起、日志、启动命令、删除等条目
状态一行显示当前是运行中、已暂停、已挂起还是未启动

### 控制台

- 键盘输入直接送进虚拟机串口
- Ctrl+] 退回菜单，虚拟机继续运行
- Ctrl+ALT+Q 同样退出控制台
- 同一台虚拟机重连保留屏幕历史，换一台才清屏
- 底部一条是状态栏，显示虚拟机名、架构、VNC 和 SSH 端口

### 输入与确认

- 输入框在底行，黄字提示，不用另开窗口
- 确认框也在底行，Enter 或空格是取消，其他键是继续
- 报错红底白字短暂停留，成功绿色，停止类提示黄色 `[已停止]`

## 文件与路径

| 内容 | 位置 |
|---|---|
| 虚拟机配置 | `~/start/<名字>.sh` |
| 固件目录 | `~/start/firmware` |
| 运行文件 | `<磁盘目录>/<磁盘名去掉后缀>/` |
| 没有磁盘时 | `~/start/<虚拟机名>/` |
| 旧目录 | `~/.vvm`，现在只在需要固件时创建 |
| socket | 优先运行目录，路径过长或 FUSE 不支持时退到 `$TMPDIR`、`/tmp`、`~` |

运行目录里放 pid、串口日志、qemu 标准输出日志、QMP socket、串口 socket、挂起标记、截图等
磁盘放在 `/storage/emulated/0/...` 时不能 bind AF_UNIX，socket 会自动退到 TMPDIR

## 配置文件

一台虚拟机一个 shell 变量文件，内容是 `VM_` 开头的赋值，加载时用 shell 语法解析，名字里带空格也可以

主要字段：

```
VM_ARCH / VM_MACHINE / VM_CPU / VM_CORES / VM_THREADS / VM_MEM
VM_GPU / VM_AUDIO / VM_IF_TYPE / VM_NVIRT
VM_FIRMWARE / VM_BIOS_FILE / VM_CODE_FILE / VM_VARS_FILE
VM_HDA / VM_HDA2 / VM_CDROM / VM_CDROM2 / VM_FDA / VM_FDB
VM_SHARED / VM_SHARED_PATH
VM_NIC / VM_NET_USER / VM_SSH_PORT / VM_DNS1 / VM_DNS2
VM_BOOT / VM_BOOT_MENU
VM_DISPLAY / VM_VNC_EXT / VM_VNC_ADDR / VM_VNC_PORT / VM_VNC_PASS / VM_SPICE_PORT
VM_MEM_AUTO / VM_LOCALTIME / VM_OVERCOMMIT / VM_ARGS
```

`VM_ARGS` 是原样追加的自定义参数，写错 qemu 会直接报错退出

## 命令行

```
vmqemu                      打开菜单
vmqemu --command [名字]     打印前台启动命令，不执行
vmqemu --command-bg [名字]  打印后台启动命令，不执行
vmqemu --run [名字]         前台启动
vmqemu --run-bg [名字]      后台启动
vmqemu --firmware           列出固件状态和目录
vmqemu --version            只打印版本号
```

不给名字时用列表里第一台
`--command` 打印的就是界面实际执行的那条命令，可以直接复制到终端排查

## 串口控制台与 VNC

- 串口固定用 `-chardev socket` 加 `-serial chardev`，日志写文件，界面通过 socket 连接
- 虚拟机没有向串口输出时，控制台显示一段说明，按回车可能才会刷出内容
- 这种情况不等于 VNC 没画面，可以切到 `VNC:127.0.0.1:5901` 看
- 串口没接出来的旧进程，按 b 会提示先用 VNC，退出后重新启动即可

## 固件

- ARM：`edk2-aarch64-code.fd`，大小应在 60–70 MB，变量盘同为 64 MB
- x86：`OVMF_CODE.fd` / `edk2-x86_64-code.fd`，变量盘按 CODE 实际大小生成，必要时拷系统 OVMF vars 模板
- x86 默认 BIOS 用 QEMU 自带的 SeaBIOS，`bios.bin` 显示为内置，不需要额外文件
- Termux 里 `pkg install qemu-system-aarch64-headless` 装好的固件会自动使用，不用手工拷贝
- 自定义固件放 `~/start/firmware`，名字对上就会优先使用

## 挂起与快照

- 在线快照走 QMP `savevm`，失败会返回 qemu 的错误原文
- 挂起到磁盘需要磁盘支持快照，qcow2 可以，raw 不行
- pflash 可写变量盘会让 `savevm` 失败，这是 qemu 限制
- 挂起前会检查磁盘空间，过程在界面里按秒刷新

## 退出码

| 码 | 含义 |
|---|---|
| 0 | 正常 |
| 1 | 用户取消 |
| 2 | 缺依赖 |
| 3 | 文件不存在 |
| 4 | 文件损坏 |
| 5 | qemu 执行失败 |
| 6 | 参数不合法 |

## 已知限制

- 纯 TUI，依赖终端支持宽字符和颜色
- 挂起、快照受 qemu 和设备类型限制，不是所有组合都能用
- Termux 的 FUSE 存储不支持 AF_UNIX bind，socket 会落到 TMPDIR
- 固件、内核、qemu 版本升级后建议重跑一次环境自检

## 版本

v1.41
