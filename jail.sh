#!/bin/bash
#
# macOS 27 chroot jail manager.
#
#   ./jail.sh build              deploy ~/.macos-jail/rootfs
#   ./jail.sh start [command]    enter the jail, or run a shell command inside
#   ./jail.sh stop               stop jail-authd and unmount the jail's devfs
#
# The jail uses only macOS's own command-line tools (no GNU coreutils).
# It runs as the invoking user (same name/uid/gid), with an empty home at
# /Users/<user> inside the jail.
#
set -u

# ---- identify the invoking user (works directly or through sudo) ----------
if [ "$(/usr/bin/id -u)" -eq 0 ]; then
    if [ -n "${JAIL_USER:-}" ]; then
        :
    elif [ -n "${SUDO_USER:-}" ]; then
        JAIL_USER=$SUDO_USER
    else
        JAIL_USER=$(/usr/bin/stat -f %Su "$0")
    fi
    if [ -n "${JAIL_HOME:-}" ]; then
        :
    else
        JAIL_HOME=$(eval echo "~$JAIL_USER")
    fi
else
    JAIL_USER=$(/usr/bin/id -un)
    JAIL_HOME=$HOME
fi

JAIL_UID=$(/usr/bin/id -u "$JAIL_USER")
JAIL_GID=$(/usr/bin/id -g "$JAIL_USER")
JAIL_GROUPS=$(/usr/bin/id -G "$JAIL_USER" | /usr/bin/tr ' ' ',')
JAIL_GROUP_NAMES=$(/usr/bin/id -Gn "$JAIL_USER")

# The current working directory IS the jail root.
ROOT="$PWD"
LAUNCHER="$ROOT/jail-launcher"
LAUNCHER_C="$ROOT/jail-launcher.c"
AUTH_C="$ROOT/jail-auth.c"
AUTHD="$ROOT/jail-authd"
PASSWD_HELPER="$ROOT/jail-passwd"
AUTH_SOCK="$ROOT/var/run/jail-auth.sock"
AUTH_PID="$ROOT/var/run/jail-authd.pid"
AUTH_SHADOW="$ROOT/etc/jail-shadow"
AUTH_LOG="$ROOT/var/log/jail-authd.log"
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

usage() {
    cat <<EOF
usage: $0 <command>
  build                       deploy the current directory as the jail root
  start [command]             enter the jail, or run a shell command inside
  install-cmdlinetools        install Apple Command Line Tools into the jail
  set-user-password [pwd]     set the container password (--clear to remove)
  stop                        stop jail-authd and unmount the jail's devfs
EOF
}

# Refuse obviously dangerous targets. The build replaces
# bin/usr/etc/System/dev/private/var/Users/Library in the current directory.
case "$ROOT" in
    /|/System|/Users|/private|/private/tmp|/private/var|/tmp|"$JAIL_HOME")
        echo "refusing to use $ROOT as the jail root" >&2
        echo "run this script from a dedicated directory, e.g. ~/myjail" >&2
        exit 1
        ;;
esac

# Everything below needs root (chroot, mount, codesign).
if [ "$(/usr/bin/id -u)" -ne 0 ]; then
    exec /usr/bin/sudo env JAIL_USER="$JAIL_USER" JAIL_HOME="$JAIL_HOME" \
        JAIL_TMPDIR="${TMPDIR:-}" /bin/bash "$0" "$@"
fi

umount_dev() {
    while /sbin/mount | /usr/bin/grep -Fq " on $ROOT/dev "; do
        /sbin/umount "$ROOT/dev" || return 1
    done
    return 0
}

pid_is_authd() {
    [ -n "${1:-}" ] || return 1
    cmd=$(/bin/ps -p "$1" -o command= 2>/dev/null || true)
    case "$cmd" in
        *"$AUTHD"*) return 0 ;;
        *) return 1 ;;
    esac
}

stop_authd() {
    pids=""
    if [ -f "$AUTH_PID" ]; then
        p=$(/bin/cat "$AUTH_PID" 2>/dev/null || true)
        [ -n "$p" ] && pids="$p"
    fi
    if [ -x /usr/bin/pgrep ]; then
        extra=$(/usr/bin/pgrep -f "$AUTHD --root $ROOT" 2>/dev/null || true)
        pids="$pids $extra"
    fi
    for pid in $pids; do
        if /bin/kill -0 "$pid" 2>/dev/null && pid_is_authd "$pid"; then
            /bin/kill "$pid" 2>/dev/null || true
        fi
    done
    for pid in $pids; do
        i=0
        while /bin/kill -0 "$pid" 2>/dev/null && pid_is_authd "$pid"; do
            i=$((i + 1))
            if [ "$i" -ge 20 ]; then
                /bin/kill -9 "$pid" 2>/dev/null || true
            fi
            if [ "$i" -ge 30 ]; then
                break
            fi
            /bin/sleep 0.1
        done
    done
    /bin/rm -f "$AUTH_PID" "$AUTH_SOCK" "$ROOT/var/run/mDNSResponder" \
                "$ROOT/var/run/jail-authd.lock"
}

start_authd() {
    if [ ! -x "$AUTHD" ]; then
        return 0
    fi
    /bin/mkdir -p "$ROOT/var/run" "$ROOT/var/log"

    if [ -f "$AUTH_PID" ]; then
        pid=$(/bin/cat "$AUTH_PID" 2>/dev/null || true)
        if [ -n "$pid" ] && /bin/kill -0 "$pid" 2>/dev/null && pid_is_authd "$pid"; then
            if [ -S "$AUTH_SOCK" ]; then
                return 0
            fi
            # Alive but no socket: broken instance; replace it.
            /bin/kill "$pid" 2>/dev/null || true
            i=0
            while /bin/kill -0 "$pid" 2>/dev/null && [ "$i" -lt 20 ]; do
                /bin/sleep 0.1
                i=$((i + 1))
            done
            /bin/kill -9 "$pid" 2>/dev/null || true
        fi
        /bin/rm -f "$AUTH_PID"
    fi

    # Do not unlink AUTH_SOCK here: a live, untracked daemon may own it.
    # The new daemon takes the fcntl lock before unlinking/binding.
    : > "$AUTH_LOG" 2>/dev/null || true
    /bin/chmod 600 "$AUTH_LOG" 2>/dev/null || true
    "$AUTHD" --root "$ROOT" --socket "$AUTH_SOCK" --shadow "$AUTH_SHADOW" \
             --uid "$JAIL_UID" --gid "$JAIL_GID" >>"$AUTH_LOG" 2>&1 &
    daemon_pid=$!
    /bin/echo "$daemon_pid" > "$AUTH_PID"
    i=0
    while [ ! -S "$AUTH_SOCK" ] && [ "$i" -lt 50 ]; do
        /bin/sleep 0.1
        i=$((i + 1))
    done
    if [ -S "$AUTH_SOCK" ]; then
        if ! /bin/kill -0 "$daemon_pid" 2>/dev/null; then
            # Another start won the daemon lock; adopt a live winner.
            winner=""
            for p in $(/usr/bin/pgrep -f "$AUTHD --root $ROOT" 2>/dev/null || true); do
                if /bin/kill -0 "$p" 2>/dev/null && pid_is_authd "$p"; then
                    winner=$p
                    break
                fi
            done
            if [ -n "$winner" ]; then
                /bin/echo "$winner" > "$AUTH_PID"
            else
                /bin/rm -f "$AUTH_PID"
            fi
        fi
        return 0
    fi
    echo "warning: jail-authd failed to start" >&2
    /usr/bin/tail -n 5 "$AUTH_LOG" >&2 2>/dev/null || true
    /bin/kill "$daemon_pid" 2>/dev/null || true
    /bin/rm -f "$AUTH_PID"
}

copy_tool() {
    src=$1
    dst=$2
    /bin/mkdir -p "$ROOT/$(/usr/bin/dirname "$dst")"
    /bin/cp -L "$src" "$ROOT/$dst" || return 1
    /bin/chmod 755 "$ROOT/$dst"
    /usr/bin/codesign --force --sign - "$ROOT/$dst" >/dev/null 2>&1 || \
        echo "warning: codesign failed for $dst" >&2
}

# ---- Command Line Tools ----------------------------------------------------
host_clt_path() {
    for p in /Library/Developer/CommandLineTools \
             "$(/usr/bin/xcode-select -p 2>/dev/null || true)"; do
        case "$p" in
            */CommandLineTools)
                if [ -x "$p/usr/bin/clang" ] && [ -e "$p/usr/bin/git" ]; then
                    /bin/echo "$p"
                    return 0
                fi
                ;;
        esac
    done
    return 1
}

host_clt_version() {
    /usr/sbin/pkgutil --pkg-info com.apple.pkg.CLTools_Executables 2>/dev/null |
        /usr/bin/awk '$1 == "version:" { print $2; exit }'
}

write_host_clt_receipts() {
    version=$1
    /bin/mkdir -p "$ROOT/var/db/receipts" \
                 "$ROOT/Library/Apple/System/Library/Receipts"
    # macOS keeps receipts for components installed into the sealed system
    # under /Library/Apple/System/Library/Receipts; pkgutil reads them there.
    for f in /Library/Apple/System/Library/Receipts/com.apple.pkg.CLTools*; do
        [ -e "$f" ] || continue
        /bin/cp "$f" "$ROOT/Library/Apple/System/Library/Receipts/" 2>/dev/null || true
    done
    /usr/sbin/pkgutil --pkgs 2>/dev/null |
        /usr/bin/grep '^com\.apple\.pkg\.CLTools' |
        while IFS= read -r id; do
            tmp="$ROOT/var/db/receipts/$id.plist.tmp"
            if /usr/sbin/pkgutil --pkg-info-plist "$id" > "$tmp" 2>/dev/null; then
                /bin/mv "$tmp" "$ROOT/var/db/receipts/$id.plist"
            else
                /bin/rm -f "$tmp"
            fi
        done
    if [ ! -f "$ROOT/var/db/receipts/com.apple.pkg.CLTools_Executables.plist" ]; then
        cat > "$ROOT/var/db/receipts/com.apple.pkg.CLTools_Executables.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>install-location</key>
    <string>/</string>
    <key>install-time</key>
    <integer>$(/bin/date +%s)</integer>
    <key>pkg-version</key>
    <string>$version</string>
    <key>pkgid</key>
    <string>com.apple.pkg.CLTools_Executables</string>
    <key>receipt-plist-version</key>
    <real>1</real>
    <key>volume</key>
    <string>/</string>
</dict>
</plist>
EOF
    fi
    /usr/sbin/chown -R root:wheel "$ROOT/var/db/receipts" \
        "$ROOT/Library/Apple/System/Library/Receipts"
}

setup_clt_in_jail() {
    /bin/mkdir -p "$ROOT/System/Library/CoreServices"
    if [ -f /System/Library/CoreServices/SystemVersion.plist ]; then
        /bin/cp /System/Library/CoreServices/SystemVersion.plist \
            "$ROOT/System/Library/CoreServices/SystemVersion.plist"
    fi

    copy_tool /usr/bin/xcode-select usr/bin/xcode-select || return 1
    copy_tool /usr/bin/xcrun        usr/bin/xcrun        || return 1
    copy_tool /usr/sbin/pkgutil     usr/sbin/pkgutil     || return 1
    copy_tool /usr/bin/sw_vers      usr/bin/sw_vers      || return 1
    copy_tool /usr/bin/install      usr/bin/install      || return 1
    copy_tool /usr/bin/plutil       usr/bin/plutil       || return 1
    copy_tool /usr/bin/chgrp        usr/bin/chgrp        || return 1
    copy_tool /usr/bin/openssl      usr/bin/openssl      || return 1
    copy_tool /usr/bin/xattr        usr/bin/xattr        || return 1
    copy_tool /usr/bin/ditto        usr/bin/ditto        || return 1

    /bin/mkdir -p "$ROOT/var/db" "$ROOT/usr/local/bin"
    /bin/ln -sfn /Library/Developer/CommandLineTools \
        "$ROOT/var/db/xcode_select_link"

    # xcrun uses confstr(DARWIN_USER_TEMP_DIR), not TMPDIR, for its cache.
    tmpbase=${JAIL_TMPDIR:-}
    if [ -z "$tmpbase" ]; then
        tmpbase=$(/usr/bin/sudo -u "$JAIL_USER" /usr/bin/getconf \
            DARWIN_USER_TEMP_DIR 2>/dev/null || true)
    fi
    case "$tmpbase" in
        /var/folders/*)
            /bin/mkdir -p "$ROOT$tmpbase"
            /usr/sbin/chown "$JAIL_UID:$JAIL_GID" "$ROOT${tmpbase%/T}" \
                2>/dev/null || true
            /bin/chmod 700 "$ROOT$tmpbase" 2>/dev/null || true
            ;;
    esac

    # Homebrew finds clang/make through PATH; the developer dir itself
    # remains the canonical location used by xcode-select/xcrun.
    for t in clang clang++ cc c++ make git-receive-pack git-upload-pack \
             git-upload-archive swift swiftc python3; do
        if [ -e "$ROOT/Library/Developer/CommandLineTools/usr/bin/$t" ]; then
            /bin/ln -sfn "/Library/Developer/CommandLineTools/usr/bin/$t" \
                "$ROOT/usr/local/bin/$t"
        fi
    done

    # A symlink changes argv[0], which makes the CLT git look for git-core
    # relative to /usr/local. Use a wrapper so remote-https helpers work.
    /bin/rm -f "$ROOT/usr/local/bin/git"
    cat > "$ROOT/usr/local/bin/git" <<'EOF'
#!/bin/bash
export GIT_EXEC_PATH=/Library/Developer/CommandLineTools/usr/libexec/git-core
export GIT_TEMPLATE_DIR=/Library/Developer/CommandLineTools/usr/share/git-core/templates
exec /Library/Developer/CommandLineTools/usr/bin/git "$@"
EOF
    /bin/chmod 755 "$ROOT/usr/local/bin/git"
    /usr/sbin/chown root:wheel "$ROOT/usr/local/bin/git"

    # The platform shasum is a Perl script and needs an interpreter that the
    # jail does not carry. Homebrew only needs SHA-256 here.
    cat > "$ROOT/usr/bin/shasum" <<'EOF'
#!/bin/bash
alg=sha1
if [ "${1:-}" = "-a" ]; then
    alg="sha$2"
    shift 2
fi
for f in "$@"; do
    h=$(/usr/bin/openssl dgst "-$alg" "$f" 2>/dev/null | /usr/bin/sed 's/.*= //')
    [ -n "$h" ] || exit 1
    /usr/bin/printf '%s  %s\n' "$h" "$f"
done
EOF
    /bin/chmod 755 "$ROOT/usr/bin/shasum"
    /usr/sbin/chown root:wheel "$ROOT/usr/bin/shasum"
}

install_clt_from_host() {
    host=$1
    version=$2
    target="$ROOT/Library/Developer/CommandLineTools"

    if [ ! -e "$target/usr/bin/clang" ]; then
        need_kb=$(/usr/bin/du -sk "$host" | /usr/bin/awk 'NR == 1 { print $1 }')
        free_kb=$(/bin/df -k "$ROOT" | /usr/bin/awk 'NR == 2 { print $4 }')
        if [ -n "$need_kb" ] && [ -n "$free_kb" ] && \
           [ "$free_kb" -lt $((need_kb + 262144)) ]; then
            echo "not enough free space in $ROOT (need about $need_kb KB)" >&2
            return 1
        fi
        /bin/mkdir -p "$ROOT/Library/Developer"
        /bin/rm -rf "$target"
        echo "copying Command Line Tools from $host ..."
        /usr/bin/ditto "$host" "$target" || {
            echo "failed to copy Command Line Tools" >&2
            return 1
        }
        /usr/sbin/chown -R root:wheel "$target" || return 1
    else
        echo "Command Line Tools directory already present; repairing links"
    fi

    setup_clt_in_jail || return 1
    write_host_clt_receipts "$version" || return 1
}

install_clt_packages() {
    installed=0
    for pkg in "$@"; do
        [ -f "$pkg" ] || continue
        case "$pkg" in
            *Remove*) continue ;;
        esac
        echo "expanding $(/usr/bin/basename "$pkg") ..."
        tmp=$(/usr/bin/mktemp -d /private/tmp/jail-clt-pkg.XXXXXX) || return 1
        if ! /usr/sbin/pkgutil --expand-full "$pkg" "$tmp" >/dev/null 2>&1; then
            echo "cannot expand $pkg" >&2
            /bin/rm -rf "$tmp"
            return 1
        fi
        if [ -d "$tmp/Payload" ]; then
            /usr/bin/ditto "$tmp/Payload/" "$ROOT/" || {
                echo "cannot install payload from $pkg" >&2
                /bin/rm -rf "$tmp"
                return 1
            }
        fi

        id=$(/usr/bin/plutil -extract identifier raw -o - "$tmp/PackageInfo" 2>/dev/null || true)
        ver=$(/usr/bin/plutil -extract version raw -o - "$tmp/PackageInfo" 2>/dev/null || true)
        if [ -n "$id" ] && [ -n "$ver" ]; then
            /bin/mkdir -p "$ROOT/var/db/receipts"
            cat > "$ROOT/var/db/receipts/$id.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>install-location</key>
    <string>/</string>
    <key>install-time</key>
    <integer>$(/bin/date +%s)</integer>
    <key>pkg-version</key>
    <string>$ver</string>
    <key>pkgid</key>
    <string>$id</string>
    <key>receipt-plist-version</key>
    <real>1</real>
    <key>volume</key>
    <string>/</string>
</dict>
</plist>
EOF
            [ -f "$tmp/Bom" ] && \
                /bin/cp "$tmp/Bom" "$ROOT/var/db/receipts/$id.bom"
            /bin/mkdir -p "$ROOT/Library/Apple/System/Library/Receipts"
            /bin/cp "$ROOT/var/db/receipts/$id.plist" \
                "$ROOT/Library/Apple/System/Library/Receipts/"
            [ -f "$ROOT/var/db/receipts/$id.bom" ] && \
                /bin/cp "$ROOT/var/db/receipts/$id.bom" \
                    "$ROOT/Library/Apple/System/Library/Receipts/"
        fi
        /bin/rm -rf "$tmp"
        installed=$((installed + 1))
    done

    if [ "$installed" -eq 0 ] || \
       [ ! -e "$ROOT/Library/Developer/CommandLineTools/usr/bin/clang" ]; then
        echo "no usable Command Line Tools payload was installed" >&2
        return 1
    fi
    /usr/sbin/chown -R root:wheel "$ROOT/Library/Developer/CommandLineTools" \
        2>/dev/null || true
    /usr/sbin/chown -R root:wheel "$ROOT/var/db/receipts" \
        "$ROOT/Library/Apple/System/Library/Receipts" 2>/dev/null || true
    setup_clt_in_jail || return 1
}

download_clt_from_apple() {
    echo "host Command Line Tools not found; querying Apple Software Update ..."
    placeholder=/tmp/.com.apple.dt.CommandLineTools.installondemand.in-progress
    marker=$(/usr/bin/mktemp /private/tmp/jail-clt-marker.XXXXXX) || return 1
    /usr/bin/touch "$placeholder" 2>/dev/null || true
    list=$(/usr/sbin/softwareupdate --list 2>&1 || true)
    /bin/rm -f "$placeholder"
    label=$(printf '%s\n' "$list" |
        /usr/bin/grep -B1 -E 'Command Line Tools' |
        /usr/bin/sed -n 's/.*Label: //p' |
        /usr/bin/head -n 1 |
        /usr/bin/sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    if [ -z "$label" ]; then
        /bin/rm -f "$marker"
        echo "Apple Software Update did not offer Command Line Tools." >&2
        echo "Install them on the host with: xcode-select --install" >&2
        echo "then rerun: $0 install-cmdlinetools" >&2
        return 1
    fi
    echo "downloading $label ..."
    if ! /usr/sbin/softwareupdate --download "$label"; then
        /bin/rm -f "$marker"
        echo "softwareupdate download failed" >&2
        return 1
    fi
    pkgs=$(/usr/bin/find /Library/Updates -type f -name 'CLTools_*.pkg' \
        -newer "$marker" 2>/dev/null || true)
    /bin/rm -f "$marker"
    if [ -z "$pkgs" ]; then
        pkgs=$(/usr/bin/find /Library/Updates -type f -name 'CLTools_*.pkg' \
            2>/dev/null || true)
    fi
    if [ -z "$pkgs" ]; then
        echo "downloaded Command Line Tools package not found" >&2
        return 1
    fi
    # shellcheck disable=SC2086
    install_clt_packages $pkgs
}

install_cmdlinetools() {
    if [ ! -x "$ROOT/bin/bash" ] || [ ! -x "$LAUNCHER" ]; then
        echo "jail is not built; run: $0 build" >&2
        exit 1
    fi

    if [ -x "$ROOT/Library/Developer/CommandLineTools/usr/bin/clang" ]; then
        echo "Command Line Tools already present in the jail"
        setup_clt_in_jail || exit 1
        if host=$(host_clt_path); then
            version=$(host_clt_version)
            [ -n "$version" ] || version=unknown
            write_host_clt_receipts "$version" || exit 1
        fi
        return 0
    fi

    if host=$(host_clt_path); then
        version=$(host_clt_version)
        [ -n "$version" ] || version=unknown
        echo "found host Command Line Tools: $host (version $version)"
        install_clt_from_host "$host" "$version" || exit 1
    else
        download_clt_from_apple || exit 1
    fi

    echo "Command Line Tools installed in:"
    echo "  $ROOT/Library/Developer/CommandLineTools"
    echo "  xcode-select: $ROOT/usr/bin/xcode-select"
}

write_home_configs() {
    home=$1
    owner=$2
    prompt=$3
    {
        echo '# Initial jail profile'
        echo 'if [ -n "$BASH_VERSION" ] && [ -f "$HOME/.bashrc" ]; then'
        echo '    . "$HOME/.bashrc"'
        echo 'fi'
    } > "$home/.profile"
    {
        echo '# Initial jail bashrc'
        echo '# PS1 style captured from the host at build time'
        printf 'PS1=%q\n' "$prompt"
    } > "$home/.bashrc"
    /usr/sbin/chown "$owner" "$home/.profile" "$home/.bashrc"
    /bin/chmod 644 "$home/.profile" "$home/.bashrc"
}

build_jail() {
    stop_authd
    if ! umount_dev; then
        echo "cannot unmount $ROOT/dev (jail still in use?)" >&2
        exit 1
    fi
    # Replace only rootfs-owned entries. Keep jail.sh, jail-launcher.c,
    # jail-auth.c, jail-launcher, README.md, .git, etc. in the current directory.
    /bin/rm -rf "$ROOT/bin" "$ROOT/usr" "$ROOT/etc" "$ROOT/System" \
                "$ROOT/dev" "$ROOT/private" "$ROOT/var" "$ROOT/Users" \
                "$ROOT/sbin" "$ROOT/opt" "$ROOT/tmp" "$ROOT/Library"
    /bin/mkdir -p "$ROOT"/{bin,sbin,usr/bin,usr/sbin,usr/local/bin,usr/lib,System/Library/dyld,dev,etc,private/tmp,private/var/tmp,var/root,var/run,var/log,var/db/timezone,Users,usr/share/terminfo}

    # ---- bash: platform binaries are rejected in chroot, so ad-hoc sign ----
    /bin/cp /bin/bash "$ROOT/bin/bash"
    /bin/chmod 755 "$ROOT/bin/bash"
    /usr/bin/codesign --force --sign - "$ROOT/bin/bash" >/dev/null
    /bin/ln -sfn bash "$ROOT/bin/sh"

    # ---- dyld (resolved inside the new root) ----
    /bin/cp /usr/lib/dyld "$ROOT/usr/lib/dyld"

    # ---- macOS command-line tools (copy + ad-hoc sign each) ----
    copy_tool /bin/ls            bin/ls
    copy_tool /bin/cat           bin/cat
    copy_tool /bin/cp            bin/cp
    copy_tool /bin/mv            bin/mv
    copy_tool /bin/rm            bin/rm
    copy_tool /bin/mkdir         bin/mkdir
    copy_tool /bin/rmdir         bin/rmdir
    copy_tool /bin/ln            bin/ln
    copy_tool /bin/chmod         bin/chmod
    copy_tool /bin/date          bin/date
    copy_tool /bin/df            bin/df
    copy_tool /bin/pwd           bin/pwd
    copy_tool /bin/sleep         bin/sleep
    copy_tool /bin/unlink        bin/unlink
    copy_tool /bin/expr          bin/expr
    copy_tool /bin/realpath      bin/realpath
    copy_tool /bin/echo          bin/echo
    copy_tool /bin/kill          bin/kill
    copy_tool /bin/stty          bin/stty

    copy_tool /usr/bin/touch     usr/bin/touch
    copy_tool /usr/bin/stat      usr/bin/stat
    copy_tool /usr/bin/readlink  usr/bin/readlink
    copy_tool /usr/bin/head      usr/bin/head
    copy_tool /usr/bin/tail      usr/bin/tail
    copy_tool /usr/bin/sort      usr/bin/sort
    copy_tool /usr/bin/uniq      usr/bin/uniq
    copy_tool /usr/bin/cut       usr/bin/cut
    copy_tool /usr/bin/tr        usr/bin/tr
    copy_tool /usr/bin/wc        usr/bin/wc
    copy_tool /usr/bin/grep      usr/bin/grep
    copy_tool /usr/bin/sed       usr/bin/sed
    copy_tool /usr/bin/awk       usr/bin/awk
    copy_tool /usr/bin/find      usr/bin/find
    copy_tool /usr/bin/xargs     usr/bin/xargs
    copy_tool /usr/bin/id        usr/bin/id
    copy_tool /usr/bin/whoami    usr/bin/whoami
    copy_tool /usr/bin/uname     usr/bin/uname
    copy_tool /usr/bin/tty       usr/bin/tty
    copy_tool /usr/bin/du        usr/bin/du
    copy_tool /usr/bin/tar       usr/bin/tar
    copy_tool /usr/bin/gzip      usr/bin/gzip
    copy_tool /usr/bin/env       usr/bin/env
    copy_tool /usr/bin/printenv  usr/bin/printenv
    copy_tool /usr/bin/mktemp    usr/bin/mktemp
    copy_tool /usr/bin/basename  usr/bin/basename
    copy_tool /usr/bin/dirname   usr/bin/dirname
    copy_tool /usr/bin/tee       usr/bin/tee
    copy_tool /usr/bin/cmp       usr/bin/cmp
    copy_tool /usr/bin/comm      usr/bin/comm
    copy_tool /usr/bin/paste     usr/bin/paste
    copy_tool /usr/bin/yes       usr/bin/yes
    copy_tool /usr/bin/printf    usr/bin/printf
    copy_tool /usr/bin/true      usr/bin/true
    copy_tool /usr/bin/false     usr/bin/false
    copy_tool /usr/sbin/chown    usr/sbin/chown
    copy_tool /sbin/ping         sbin/ping
    copy_tool /usr/bin/curl      usr/bin/curl
    copy_tool /usr/bin/nc        usr/bin/nc
    copy_tool /usr/bin/nano      usr/bin/nano

    # ---- terminfo (for ls --color, stty, etc.) ----
    for e in x/xterm-256color x/xterm v/vt100 a/ansi s/screen s/screen-256color; do
        if [ -f "/usr/share/terminfo/$e" ]; then
            /bin/mkdir -p "$ROOT/usr/share/terminfo/$(/usr/bin/dirname "$e")"
            /bin/cp "/usr/share/terminfo/$e" "$ROOT/usr/share/terminfo/$e"
        fi
    done

    # ---- users / groups: same name, uid, gid as the invoking user ----
    {
        echo "root:*:0:0:System Administrator:/var/root:/bin/bash"
        echo "$JAIL_USER:*:$JAIL_UID:$JAIL_GID:$JAIL_USER:/Users/$JAIL_USER:/bin/bash"
    } > "$ROOT/etc/passwd"

    {
        echo "wheel:*:0:root"
        names=($JAIL_GROUP_NAMES)
        gids=($(/bin/echo "$JAIL_GROUPS" | /usr/bin/tr ',' ' '))
        for i in "${!gids[@]}"; do
            gid=${gids[$i]}
            [ "$gid" = "0" ] && continue
            name=${names[$i]:-gid$gid}
            echo "$name:*:$gid:$JAIL_USER"
        done
    } > "$ROOT/etc/group"

    echo "127.0.0.1 localhost" > "$ROOT/etc/hosts"
    for f in /etc/services /etc/protocols /etc/ssl/cert.pem /etc/resolv.conf; do
        if [ -f "$f" ]; then
            /bin/mkdir -p "$ROOT$(/usr/bin/dirname "$f")"
            /bin/cp -L "$f" "$ROOT$f"
        fi
    done
    {
        echo '# System-wide profile for the jail.'
        echo '# Per-user prompt is configured in ~/.profile and ~/.bashrc.'
    } > "$ROOT/etc/profile"
    if [ -f /etc/inputrc ]; then
        /bin/cp /etc/inputrc "$ROOT/etc/inputrc"
    fi

    # ---- launcher config ----
    printf '%s %s %s %s\n' "$JAIL_USER" "$JAIL_UID" "$JAIL_GID" "${JAIL_GROUPS:--}" \
        > "$ROOT/etc/jail-user"
    /bin/chmod 644 "$ROOT/etc/jail-user"

    # ---- home / tmp / root ----
    /bin/mkdir -p "$ROOT/Users/$JAIL_USER"
    /usr/sbin/chown "$JAIL_UID:$JAIL_GID" "$ROOT/Users/$JAIL_USER"
    /bin/chmod 700 "$ROOT/Users/$JAIL_USER"
    /bin/chmod 700 "$ROOT/var/root"
    /bin/chmod 1777 "$ROOT/private/tmp" "$ROOT/private/var/tmp"

    # ---- initial profile/bashrc using the host user's prompt style ----
    host_ps1=$(/usr/bin/sudo -H -u "$JAIL_USER" /bin/bash -ic \
        'printf "__PS1__%s__END__" "$PS1"' 2>/dev/null || true)
    case "$host_ps1" in
        *__PS1__*__END__*)
            host_ps1=${host_ps1#*__PS1__}
            host_ps1=${host_ps1%%__END__*}
            ;;
        *)
            host_ps1='\h:\W \u\$ '
            ;;
    esac
    [ -n "$host_ps1" ] || host_ps1='\h:\W \u\$ '
    write_home_configs "$ROOT/Users/$JAIL_USER" "$JAIL_UID:$JAIL_GID" "$host_ps1"
    write_home_configs "$ROOT/var/root" "0:0" "$host_ps1"

    # ---- timezone ----
    lt=$(/usr/bin/readlink /etc/localtime 2>/dev/null || true)
    if [ -n "$lt" ] && [ -f "$lt" ]; then
        /bin/mkdir -p "$ROOT$(/usr/bin/dirname "$lt")"
        /bin/cp "$lt" "$ROOT$lt"
        /bin/ln -sfn "$lt" "$ROOT/etc/localtime"
    elif [ -f /etc/localtime ]; then
        /bin/cp /etc/localtime "$ROOT/etc/localtime"
    fi
    /bin/ln -sfn /var/db/timezone/zoneinfo "$ROOT/usr/share/zoneinfo"

    # ---- simulated su/sudo: root helper daemon + clients ----
    if [ ! -f "$AUTH_C" ]; then
        for cand in "$SCRIPT_DIR/jail-auth.c" \
                    "$JAIL_HOME/macos-jail/jail-auth.c"; do
            if [ -f "$cand" ]; then
                /bin/cp "$cand" "$AUTH_C"
                break
            fi
        done
    fi
    if [ ! -f "$AUTH_C" ]; then
        echo "missing auth source: $AUTH_C" >&2
        echo "put jail-auth.c next to this script or in the current directory" >&2
        exit 1
    fi
    /usr/bin/clang -O2 -o "$ROOT/jail-auth" "$AUTH_C" || exit 1
    /bin/cp "$ROOT/jail-auth" "$AUTHD"
    /bin/cp "$ROOT/jail-auth" "$PASSWD_HELPER"
    /bin/cp "$ROOT/jail-auth" "$ROOT/usr/local/bin/sudo"
    /bin/cp "$ROOT/jail-auth" "$ROOT/usr/local/bin/su"
    # Keep clang's linker-signed ad-hoc signature. Re-signing copies named
    # sudo/su makes taskgated treat them as (invalid) system sudo/su.
    # Also keep them OUT of /usr/bin: macOS 27 applies a special policy to
    # the exact path /usr/bin/sudo even inside a chroot.
    for f in "$AUTHD" "$PASSWD_HELPER" "$ROOT/usr/local/bin/sudo" "$ROOT/usr/local/bin/su"; do
        /bin/chmod 755 "$f"
    done
    /bin/rm -f "$ROOT/jail-auth" "$ROOT/.jail-auth"

    # ---- compile the C launcher ----
    # Prefer jail-launcher.c already in the current directory; otherwise
    # pick it up from the script directory or an existing deployment.
    if [ ! -f "$LAUNCHER_C" ]; then
        for cand in "$SCRIPT_DIR/jail-launcher.c" \
                    "$JAIL_HOME/.macos-jail/jail-launcher.c" \
                    "$JAIL_HOME/macos-jail/jail-launcher.c"; do
            if [ -f "$cand" ]; then
                /bin/cp "$cand" "$LAUNCHER_C"
                break
            fi
        done
    fi
    if [ ! -f "$LAUNCHER_C" ]; then
        echo "missing launcher source: $LAUNCHER_C" >&2
        echo "put jail-launcher.c next to this script or in the current directory" >&2
        exit 1
    fi
    /usr/bin/clang -O2 -o "$LAUNCHER" "$LAUNCHER_C" || exit 1

    echo "built:    $ROOT"
    echo "user:     $JAIL_USER ($JAIL_UID:$JAIL_GID)"
    echo "launcher: $LAUNCHER"
}

start_jail() {
    if [ ! -x "$LAUNCHER" ] || [ ! -x "$ROOT/bin/bash" ]; then
        echo "jail is not built; run: $0 build" >&2
        exit 1
    fi
    start_authd
    export JAIL_ROOT="$ROOT"
    if [ $# -eq 0 ]; then
        exec "$LAUNCHER"
    fi
    exec "$LAUNCHER" /bin/bash -c "$*"
}

set_password() {
    if [ "${1:-}" = "--clear" ]; then
        /bin/rm -f "$AUTH_SHADOW"
        echo "password cleared; su/sudo disabled for $JAIL_USER"
        return 0
    fi
    if [ $# -ge 1 ]; then
        pw=$1
    else
        if [ ! -t 0 ]; then
            echo "no tty; pass the password as an argument" >&2
            return 1
        fi
        IFS= read -r -s -p "New password: " pw
        echo
        IFS= read -r -s -p "Retype password: " pw2
        echo
        if [ "$pw" != "$pw2" ]; then
            echo "passwords do not match" >&2
            return 1
        fi
    fi
    if [ -z "$pw" ]; then
        echo "empty password not allowed" >&2
        return 1
    fi
    if [ ! -x "$PASSWD_HELPER" ]; then
        echo "jail is not built; run: $0 build" >&2
        return 1
    fi
    if printf '%s' "$pw" | "$PASSWD_HELPER" "$AUTH_SHADOW"; then
        echo "password set for $JAIL_USER; su/sudo enabled"
    else
        echo "failed to set password" >&2
        return 1
    fi
}

case "${1:-}" in
    build)
        build_jail
        ;;
    start)
        shift
        start_jail "$@"
        ;;
    install-cmdlinetools)
        install_cmdlinetools
        ;;
    set-user-password)
        shift
        set_password "$@"
        ;;
    stop)
        stop_authd
        if umount_dev; then
            echo "stopped jail-authd; unmounted $ROOT/dev"
        else
            echo "jail-authd stopped, but unmount failed (busy?)" >&2
            exit 1
        fi
        ;;
    *)
        usage
        exit 1
        ;;
esac
