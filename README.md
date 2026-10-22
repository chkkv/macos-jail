# macOS 27 chroot jail

仓库：<https://github.com/chkkv/macos-jail>

**运行 `jail.sh` 时的当前目录就是 rootfs 根目录。**
`build`、`start` 和 `stop` 都以当前工作目录为准，不再使用 `~/.macos-jail`。

## 从仓库部署

```sh
git clone https://github.com/chkkv/macos-jail.git
cd macos-jail
./jail.sh build
./jail.sh start
```

`build` 会在当前目录生成 `bin/ usr/ etc/ System/ dev/ private/ var/ Users/`，
仓库里的 `jail.sh`、`jail-launcher.c`、`jail-auth.c`、`README.md`、`.git` 会保留。
生成的 rootfs 目录已加入 `.gitignore`。

## 在独立目录部署（推荐）

```sh
mkdir ~/myjail
cd ~/myjail
/path/to/macos-jail/jail.sh build
/path/to/macos-jail/jail.sh start
```

也可以把 `jail.sh`、`jail-launcher.c`、`jail-auth.c` 复制进 `~/myjail`，然后：

```sh
cd ~/myjail
./jail.sh build
./jail.sh start
```

## 使用

```sh
./jail.sh build                       # 在当前目录部署/重建 rootfs
./jail.sh install-cmdlinetools        # 安装 Apple Command Line Tools
./jail.sh set-user-password           # 交互设置容器密码
./jail.sh set-user-password 'secret'  # 直接设置容器密码
./jail.sh set-user-password --clear   # 清除密码，禁用 su/sudo
./jail.sh start                       # 进入交互式 jail（bash -l）
./jail.sh start 'id; ls -la /'        # 在 jail 内执行 shell 命令
./jail.sh stop                        # 停止 jail-authd 并卸载 devfs
```

`build`、`start`、`set-user-password`、`stop` 需要 root，脚本会自动通过 `sudo` 重新执行。
`start`/`stop` 必须在已部署的 rootfs 目录中运行。

脚本会拒绝在以下位置部署/操作，避免误删：

```text
/  /System  /Users  /private  /private/tmp  /private/var  /tmp  $HOME
```

请使用独立目录，例如 `~/myjail`。

## 目录结构

```text
<当前目录>/
├── bin/  usr/  etc/  System/  dev/  private/  var/  Users/   # rootfs
│   ├── usr/local/bin/sudo  usr/local/bin/su                  # 模拟客户端
│   ├── Users/<user>/.profile  Users/<user>/.bashrc           # 初始配置
│   ├── var/root/.profile      var/root/.bashrc               # root 初始配置
│   ├── var/run/jail-auth.sock                                # 运行时
│   ├── var/run/mDNSResponder                                 # DNS 代理 socket
│   └── etc/jail-shadow                                       # 密码哈希
├── jail.sh
├── jail-launcher.c
├── jail-launcher
├── jail-auth.c
├── jail-authd
├── jail-passwd
└── README.md
```

`build` 只替换 rootfs 自己的目录：

```text
bin usr etc System dev private var Users sbin opt tmp Library
```

`jail.sh`、`jail-launcher.c`、`jail-auth.c`、`jail-launcher`、
`jail-authd`、`jail-passwd`、`README.md`、`.git` 会保留。
因此可以在仓库目录或已有脚本的目录里重复执行 `build`。

## 特性

- 使用 **macOS 自带命令行工具**，不包含 GNU coreutils。
- jail 内用户与宿主机当前用户一致：名称、uid、主 gid、附加组都相同。
  - 当前环境：`chkkv` / uid 501 / gid 20
  - jail 内 home：`/Users/chkkv`（属于 501:20）
  - `/etc/passwd`、`/etc/group` 由 build 时检测到的用户信息生成。
- build 会写入初始 `~/.profile` 和 `~/.bashrc`（jail 用户和 root 各一套）。
  - PS1 在 build 时从宿主机当前用户的交互式 bash 捕获，保持本机样式。
  - `.profile` 会 source `.bashrc`，login shell 和交互式 non-login shell 都能生效。
- 进入 jail 的初始 cwd 是 jail 用户的 home（由 launcher 设置，**不写入 `.profile`**）。
  - `jail.sh start` 和 `jail.sh start '<command>'` 都从 home 开始。
  - 之后在 `/`、`/private/tmp` 等目录执行 `bash -l` 不会被强制切回家。
- 启动后自动降权到该用户，`id`/`whoami` 与宿主机一致。
- 包含的工具：
  `ls cat cp mv rm mkdir rmdir ln chmod chown date df pwd sleep unlink expr
  realpath echo kill touch stat readlink head tail sort uniq cut tr wc grep
  sed awk find xargs id whoami uname du tar gzip env printenv mktemp
  basename dirname tee cmp comm paste yes printf true false stty tty
  ping curl nc nano`
- 模拟 `sudo`/`su`（见下节），客户端位于 `/usr/local/bin`，不依赖 setuid。
- terminfo、时区（Asia/Shanghai）已复制。
- 基础 rootfs 约 21 MB；安装 Command Line Tools 后约 1.3 GB。

## 网络工具

- `ping`：`/sbin/ping`，宿主机上不是 setuid，jail 内以普通用户直接运行。
- `curl`：`/usr/bin/curl`；HTTPS 使用 `/etc/ssl/cert.pem`。
- `nc`：`/usr/bin/nc`。
- `nano`：`/usr/bin/nano`。macOS 的 `nano` 实际是 `pico` 的符号链接；
  如需 GNU nano 需要另外安装。
- `/etc/services`、`/etc/protocols` 已复制。

**DNS：**

- macOS 不使用 `/etc/resolv.conf` 做实际解析。
- `jail-authd` 在：
  ```text
  <rootfs>/var/run/mDNSResponder
  ```
  创建一个 Unix socket，把连接**双向转发**到宿主：
  ```text
  /var/run/mDNSResponder
  ```
- 转发同时保留 `SCM_RIGHTS` ancillary fd（mDNSResponder 用它传 reply socket），
  所以不是普通字节流代理。
- 因此**不需要硬链接，也不要求 rootfs 与 `/var/run` 同卷**；外部卷也可以。
- daemon 必须运行：`start` 启动它，`stop` 停止并删除代理 socket。
- mDNSResponder 重启后，新连接会自动连到新的宿主 socket，不需要重建链接。

## Command Line Tools

jail 默认只有 macOS 自带的基础工具，没有 `clang`、`make`、`git`。安装：

```sh
./jail.sh install-cmdlinetools
```

行为：

- 先检查宿主机的 `/Library/Developer/CommandLineTools`。
- 存在时直接复制进 jail 的同一路径（约 1.3 GB，APFS 上通常很快）。
- 不存在时通过 Apple Software Update 查找并下载当前系统可用的 CLT。
- 同时安装 `xcode-select`、`xcrun`、`pkgutil`、`sw_vers` 等 Homebrew
  会检查的系统工具，并复制 CLT receipts 和 `SystemVersion.plist`。
- 在 `/usr/local/bin` 建立 `clang`、`make`、`git` 等入口；`git` 是包装脚本，
  会显式设置 `GIT_EXEC_PATH`，避免 `remote-https` helper 丢失。
- launcher 会在检测到 CLT 后设置 `SDKROOT`、`DEVELOPER_DIR`、`TMPDIR`。

`build` 会删除 `Library/`，因此重新 build 后需要再次运行
`install-cmdlinetools`。

## Homebrew

CLT 安装后可以安装 Homebrew。国内环境可以同时使用 USTC 和本地代理；
下面示例代理为 `127.0.0.1:10809`：

```sh
export http_proxy="http://127.0.0.1:10809"
export https_proxy="$http_proxy"
export HTTP_PROXY="$http_proxy"
export HTTPS_PROXY="$http_proxy"
export ALL_PROXY="$http_proxy"
export all_proxy="$http_proxy"
export no_proxy="localhost,127.0.0.1"

export HOMEBREW_BREW_GIT_REMOTE="https://mirrors.ustc.edu.cn/brew.git"
export HOMEBREW_API_DOMAIN="https://mirrors.ustc.edu.cn/homebrew-bottles/api"

# 不要设置 HOMEBREW_BOTTLE_DOMAIN：USTC 的 legacy flat 镜像没有
# portable-ruby，留空让 portable Ruby 走官方 GHCR（可通过代理）。
unset HOMEBREW_BOTTLE_DOMAIN
unset HOMEBREW_ARTIFACT_DOMAIN

/bin/bash -c "$(curl -fsSL https://mirrors.ustc.edu.cn/misc/brew-install.sh)"
eval "$(/opt/homebrew/bin/brew shellenv)"
```

验证：

```sh
brew --version
brew config
brew install hello
hello
```

已知问题：部分 Homebrew 安装脚本末尾的 shell here-doc 可能报
`cannot create temp file ... Permission denied`，但 CLT、portable Ruby
和 `brew` 已经安装；上面的 `brew install hello` 可以正常完成。

## 模拟 su/sudo

jail 内的 `sudo`/`su` 是普通客户端（`/usr/local/bin/sudo`、`/usr/local/bin/su`，
无 setuid）。它们连接宿主机上的 root helper `jail-authd`，校验**容器密码**后
在 jail 内以 root 执行命令。

```sh
# 宿主机：设置/清除容器密码
./jail.sh set-user-password            # 交互输入两次
./jail.sh set-user-password 'secret'   # 直接指定（会留在 shell 历史中）
./jail.sh set-user-password --clear    # 清除后 su/sudo 全部拒绝

# jail 内：
sudo id
sudo /bin/ls /
sudo -i                 # root 登录 shell
su                      # root 登录 shell
su -c 'id'              # root 执行 shell 命令
sudo -S id              # 从 stdin 读密码（脚本用）
```

- 密码独立于宿主机账户，PBKDF2-HMAC-SHA256 哈希保存在
  `<root>/etc/jail-shadow`（root:wheel 0600）。
- 未设置密码（文件不存在/为空）时，`sudo`/`su` 直接拒绝。
- `jail-authd` 由 `start` 启动、`stop` 停止；socket 为
  `<root>/var/run/jail-auth.sock`（root:staff 0660）。
- 重复执行 `start` 时，同一 rootfs 只会有一个 `jail-authd`：
  脚本复用存活 PID，daemon 自身还有 `fcntl` 写锁；并发启动时落败的一方
  会退出，脚本随后复用胜者的 PID。
- `stop` 会停止 `jail-authd` 并卸载 devfs。
- 密码错误会逐步退避（1s/2s/4s…，上限 30s）。
- 非 PTY 命令通过 fd 传递 stdio；交互式命令/`su` 使用 PTY relay，
  因此 `vim`、`top`、`less` 这类终端程序（需自行加入 jail）可以工作。
- `sudo` 保留调用者 cwd；执行环境被重置为固定 PATH/HOME。
- 只支持切换到 root；不支持 `sudo -u`、真实 PAM/opendirectory 认证。

**安全提示：**

- jail 内 root 可以通过 devfs 访问 `/dev/disk*` 等设备，因此“容器 root”
  实际上接近宿主 root。这里的密码只是提权门槛，不是安全沙箱。
- socket 在宿主机上对同一 uid 可见；任何 chkkv 进程都可以尝试密码。
- 不要在 `/usr/bin` 下放置模拟的 `sudo`/`su`：macOS 27 对
  `/usr/bin/sudo` 有特殊路径策略，会直接 SIGKILL；客户端必须使用
  `/usr/local/bin`。
- `build` 会重建 `etc/`，因此会**清除已设置的容器密码**，需要重新设置。
- `stop` 前先退出 jail 内的 root shell；否则 daemon 停止后可能仍有
  子进程占用挂载点。

## 原理

1. **bash 重签名**
   macOS 27 的 AMFI 禁止在 chroot 中执行平台二进制。build 时复制 `/bin/bash`
   并 `codesign --force --sign -`，使其成为 ad-hoc 签名副本。

2. **真实 dyld 缓存**
   不复制 2.4 GB 缓存。启动器在 `chroot()` 前打开 Preboot 卷里真实的、受 SIP
   保护的缓存文件，保持 fd 不关闭，在 jail 内建立：
   ```
   /System/Library/dyld/<name> -> /dev/fd/<n>
   ```
   jail 内 dyld 打开这些链接时仍拿到真实缓存 vnode，因此可以正常映射。

3. **devfs**
   启动器在 jail 的 `/dev` 挂 devfs（`/dev/fd` 由 devfs 提供）。
   挂载前用 `statfs` 判断，避免重复运行叠加挂载。

4. **降权**
   启动器以 root 完成 mount/open/chroot 后，读取 `/etc/jail-user`，执行
   `setgroups` / `setgid` / `setuid`，再 exec bash 或命令。

5. **模拟 su/sudo**
   launcher 和 `jail-authd` 都按固定顺序把真实缓存文件打开到 **fd 200+**，
   因此 daemon 的 root 子进程也能通过 `/dev/fd/200+` 加载 libSystem。
   daemon 校验 `/etc/jail-shadow` 中的 PBKDF2 密码，然后 chroot、以 root
   `execvp`；非 PTY 用 `SCM_RIGHTS` 传 stdio fd，交互式用 `forkpty` + PTY relay。

## 注意

- 这不是安全沙箱；chroot 内是普通用户，但 chroot 本身不是强隔离边界。
- `/dev` 是 devfs，会暴露系统设备节点。
- jail 内 home 是独立空目录，不是宿主机的 `/Users/chkkv`。
- `df` 可能打印 `getattrlist failed for /System/Volumes/Data`，但结果仍可用；
  这是 chroot 中缺少该挂载点导致的。
- `stop` 会先停止 `jail-authd`；如果 jail 内仍有进程，devfs 卸载会 busy，
  请先退出 jail 再执行。
- 修改 `jail-launcher.c` / `jail-auth.c` 后重新运行 `./jail.sh build`。
- `build` 清除 `etc/`，因此容器密码会丢失，需要重新 `set-user-password`。
- `build` 还会重建 `Users/` 和 `var/root`，home 下的 `.profile`、`.bashrc`
  以及用户文件都会被重置。
- `build` 会删除 `Library/`；重新 build 后需再次运行
  `install-cmdlinetools`。
- macOS 大版本升级后如果 dyld 缓存路径变化，需要同步修改启动器里的
  `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld`。
- 旧版本在 `~/.macos-jail` 下的部署已不再使用，可以删除。
