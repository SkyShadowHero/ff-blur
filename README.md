# ffblur — 给 Firefox 加 Wayland 背景模糊 + 半透明 + 换图标

Firefox 160 至今**没有**实现 `ext-background-effect-v1`（`libxul.so` 里连符号都没有，
[bugzilla 也搜不到](https://bugzilla.mozilla.org/rest/quicksearch?quicksearch=ext-background-effect)）。
这里不重新编译 Firefox，而是用 `LD_PRELOAD` 在 libwayland 层把缺失的那几条请求补上，
再配一份 `userChrome.css` 让窗口真的半透明。

验证环境：CachyOS / KWin 6.8.3 + Better Blur DX 2.6.0 / Firefox 160.0.1。

## 原理

背景模糊要两半都成立：

**A. 客户端请求模糊**（本仓库的 `libffblur.so`）

1. `wl_display_connect` 时用自己的 registry 做一次 roundtrip，绑定 `wl_compositor` 和
   `ext_background_effect_manager_v1`，读 `capabilities` 的 blur 位。
2. 拦截 `wl_proxy_marshal_array_flags`，跟踪 `xdg_wm_base.get_xdg_surface` →
   `xdg_surface.get_toplevel`，记住哪个 `wl_surface` 是**顶层窗口**（菜单/光标等 popup 不算）。
3. 在该 surface 的 `wl_surface.commit` **之前**插 `get_background_effect` + `set_blur_region`。
   区域取 `set_window_geometry` 的窗口尺寸（尺寸未知时先给 16384×16384，合成器会裁到
   surface 大小），窗口 resize 后自动重设。
4. 区域是双缓冲状态，必须赶在 commit 前发；模糊半径由合成器决定，客户端只说"这块要模糊"。

**B. 窗口得是半透明的**（`userChrome.css`）

surface 的 buffer 带 alpha，合成器才会把"桌面 + 模糊"合成到窗口下面。见
[窗口半透明](#窗口半透明userchromecss)。

## 快速开始

```sh
make          # 生成 libffblur.so（需要 wayland-scanner / pkg-config）
./install.sh  # 打开交互菜单
```

## 交互菜单

`./install.sh` 不带参数就是菜单，先显示当前状态，再问做什么：

```
ffx — Firefox 背景模糊 / 图标
当前状态
  菜单项  firefox.desktop  模糊=on  图标=/home/…/icons/ffx/firefox.png
  桌面项  firefox.desktop  模糊=on
  命令    /home/…/.local/bin/firefox（影子，带模糊）
  备份    1 个（还原时用）

请选择:
  1) 执行（加模糊、换图标、生成启动项）
  2) 还原（移除本工具装的东西，恢复备份）
  3) 诊断（为什么没模糊）
  4) 窗口半透明（改 userChrome.css）
  0) 退出
```

选 1 后再问具体做什么：

```
要做什么？
  1) 加模糊（保持现有图标和名称）
  2) 加模糊 + 换图标
  3) 只换图标（不加模糊）
  4) 新建一个独立启动项（不动现有 Firefox）
```

接着是图标（8 个预设 / 保持当前 / 手动路径）、名称、是否接管 `firefox` 命令、是否更新
桌面快捷方式、圆角半径，最后打一份摘要，输入 `[Y/n]` 确认才执行。

选 2 列出将移除的项和将恢复的备份，确认后删除带 `installed-by-ffx` 标记的文件、还原
`*.bak` / `*.orig`，并刷新 KDE 菜单缓存。

选 4 管理 `userChrome.css`（写入 / 移除），问不透明度、圆角、网页区是否透明。

非交互场景直接走参数：

```sh
./install.sh direct      # 等价 ./ffx install --id firefox --shadow --force
./install.sh uninstall   # 等价 ./ffx uninstall all
./install.sh --help
```

`stdin/stdout` 不是终端时（管道、cron）打印帮助而不是卡在等输入。

## ffx

```
ffx menu                                 交互式菜单（等价于 ./install.sh）
ffx run [选项] [--] [firefox 参数...]   直接启动，不写任何文件
ffx install [选项]                       生成 wrapper + .desktop
ffx uninstall <id|all>                   移除本工具生成的东西
ffx list                                 列出已生成的启动项
ffx doctor                               诊断为什么没模糊
ffx icons [目录]                         列出可用图标预设
ffx css [show|install|remove]            管理 userChrome.css 的半透明背景
ffx setup                                把 ffx 装到 PATH + 补全
ffx version / help
```

选项：

| 选项 | 说明 |
|---|---|
| `--blur` / `--no-blur` | 是否注入模糊，默认 `blur` |
| `--radius N` | `FFBLUR_RADIUS` + CSS `border-radius`，挖掉圆角 |
| `--opacity 0.8` | `ffx css` 用的背景不透明度（越小越透） |
| `--content-transparent` | `ffx css`：网页区域也透出桌面 |
| `--icon PATH\|预设名\|主题名` | 图片路径、`icons/` 里的预设名，或主题图标名。命中图片会复制进 `~/.local/share/icons/ffx/` |
| `--name NAME` | 显示名称 |
| `--id ID` | 桌面项 id，默认 `firefox-blur`；`--id firefox` 才接管原生菜单项 |
| `--profile DIR` | 指定 profile |
| `--args "…"` | 烘焙进 wrapper 的额外参数 |
| `--shadow` | 额外生成 `~/.local/bin/firefox`，让 `firefox` 命令本身带补丁 |
| `--desktop` | 额外在桌面目录生成快捷方式 |
| `--force` | 覆盖已存在且非本工具生成的文件（默认拒绝） |
| `--no-menu` | 只生成 wrapper |

生成的 `.desktop` 自带 `New Window` / `New Private Window` 两个 action，
`StartupWMClass=firefox` 保证任务栏图标归组。

### 常用例子

```sh
# 换成 momo 图标 + 桌面快捷方式 + 接管 firefox 命令
./ffx install --id firefox --shadow --force --desktop --icon momo --name Firefox

# 单独一个带模糊的启动项，不动现有 firefox
./ffx install --icon kit --name "Firefox (kit)" --radius 8

# 只加桌面快捷方式
./ffx install --no-menu --desktop --id firefox-glass --name "Firefox Glass"

# 临时跑一次，什么都不装
./ffx run --radius 8 -- --new-window https://example.com

# 不要模糊
./ffx install --no-blur --id firefox-clean --name "Firefox (no blur)"
```

### 预设图标

`icons/` 里是 Firefox 官方的 8 套图标（`momo`、`kit`、`pixelated`、`retro2004`、
`retro2017`、`pride`、`minimal-dark`、`minimal-light`），从上游
`mozilla-firefox/firefox:browser/components/shell/icons/` 拉的，并已核验与本机
`omni.ja` 内置图标逐像素一致。来源与许可见 [icons/README.md](icons/README.md)。

```sh
./ffx icons            # 列出现有预设
./ffx install --icon momo
```

`--icon` 先按预设名找，找不到才当主题图标名（`firefox` 之类）。想加自己的，把
`<名字>.png` 丢进 `icons/` 即可。

## 窗口半透明（userChrome.css）

从现有 `userChrome.css` 学到的做法，`ffx css` 把它参数化了：

1. `:root` / `#main-window` / `body` 设 `background: transparent` —— 顶层窗口不再刷底色，
   surface 因此带 alpha，桌面能透出来。
2. 外层容器（`#navigator-toolbox`、`#TabsToolbar`、`#nav-bar`、`#PersonalToolbar`、
   `#sidebar-box`、`#browser`、`.browserStack`）刷 `rgba(..., 0.8)`；浅色
   `rgba(255,255,255,a)`、深色 `rgba(0,0,0,a)` 各一套，跟随 `prefers-color-scheme`。
3. 内层容器（`#tabbrowser-tabpanels` 及其 hbox）保持透明，**避免多层 alpha 叠加**：
   `0.8` 叠 `0.8` 会变成 `0.96`，越叠越不透明。
4. 可选圆角：`#main-window { border-radius: 8px; overflow: hidden }`，和
   `ffx install --radius 8`（挖掉模糊区域的圆角）配套。

```sh
./ffx css show                              # 当前 profile / 是否启用
./ffx css install                           # 默认 0.8
./ffx css install --opacity 0.85 --radius 8
./ffx css install --content-transparent     # 网页区域也透出桌面
./ffx css remove
./ffx css install --profile /path/to/profile
```

实现细节：

- 用 `/* >>> ffx-translucent ... >>> */` / `/* <<< ffx-translucent <<< */` 标记块，
  **只替换块内内容**，你原来的规则原样保留；重复执行不会产生第二份。
- 第一次改写前备份成 `userChrome.css.ffx.bak`。
- 自动确保 `toolkit.legacyUserProfileCustomizations.stylesheets` 为 `true`（缺就写进
  `user.js`），否则 `userChrome.css` 根本不会加载。
- profile 定位：`[Install*] Default=` → `[Profile*] Default=1` 的 `Path` → 最新的
  `*.default-release`；同时支持 `~/.config/mozilla/firefox` 和 `~/.mozilla/firefox`。

网页本身有背景色时仍然不透明（这是对的，否则文字没法看）；`--content-transparent`
是给"想让桌面透过页面"的场景。

## install.sh

兼容入口，全部转给 `ffx`：

| 命令 | 等价 |
|---|---|
| `./install.sh` | `./ffx menu`（交互） |
| `./install.sh direct` | `./ffx install --id firefox --shadow --force` |
| `./install.sh uninstall` | `./ffx uninstall all` |

## 卸载

```sh
./ffx uninstall all        # 菜单项 / 桌面项 / wrapper
./ffx css remove           # userChrome.css 里的块
./ffx uninstall firefox    # 只删某一个 id
```

只删带 `installed-by-ffx` 标记的文件。补丁库 `~/.local/lib/ffblur/` 和图标
`~/.local/share/icons/ffx/` 会保留，可手动删。

## 为什么"从桌面启动还是不模糊"

`ffx doctor` 会逐项点名。常见原因：

1. 桌面上的快捷方式还是老的 `Exec=/usr/lib/firefox/firefox %u` —— 不经过补丁。
   `ffx install --desktop --force` 重写它。
2. 菜单项改了但 KDE 缓存没刷新：`kbuildsycoca6 --noincremental`，或注销重登。
3. 窗口不是半透明的 —— 没有 alpha 就看不出模糊。`ffx css install`。
4. 合成器没开 blur 效果（KWin blur / Better Blur）。
5. Firefox 已经是旧进程：Wayland 下新启动会复用现有实例，旧进程没带 `LD_PRELOAD`。
   先把 Firefox 完全退出。

## 自动让路

补丁会检测客户端是否**自己**在用这个协议（自己 bind 了 manager，或某 surface 已有 effect
对象），发现就退出，避免重复创建触发 `surface already has an associated blur object`
协议错误（这个错误会直接踢掉连接）。所以 GTK 4.23.3+ 应用、kitty 0.51 这类自带支持的程序
不会被干扰。

## 注意

- 只在原生 Wayland 窗口生效；XWayland 无效。
- 协议还在 `staging`，合成器可以不认。
- 运行时补丁：Firefox 升级后依然可用；若哪天 Firefox 自己实现了，`doctor` 会告诉你。

## 文件

```
ffx                                          启动器/桌面项/图标/CSS 管理器（主入口）
ffblur.c                                     libwayland 拦截器
icons/                                       8 套官方图标预设（ico + 256px png）
completions/                                 bash / zsh / fish 补全
ext-background-effect-v1.xml                 上游协议描述（staging）
ext-background-effect-v1-*.{h,c}             wayland-scanner 生成
Makefile  install.sh  firefox-blur
```
