# 预设图标（Firefox 官方内建图标集）

这 8 套是 Firefox 官方的「更换浏览器图标」资源（[Bug 2049877](https://bugzilla.mozilla.org/show_bug.cgi?id=2049877)
引入），上游位置：

```
mozilla-firefox/firefox  →  browser/components/shell/icons/*.ico
```

拉取时间 2026-11-24，分支 `main`，每个 `.ico` 含 16/32/48/256 四帧；
同名 `.png` 由第 4 帧（256×256）转出，供 `Icon=` 直接引用。

已逐像素比对：与 `/usr/lib/firefox/browser/omni.ja` 内 `chrome/browser/content/browser/icons/`
下的同名官方图标完全一致（`magick compare -metric AE` = 0）。

| 预设 | 说明 |
|---|---|
| `momo` | 卡通狐狸 Momo |
| `kit` | Kit |
| `pixelated` | 像素风 |
| `retro2004` | 2004 复古 |
| `retro2017` | 2017 复古 |
| `pride` | 彩虹 |
| `minimal-dark` | 极简深色 |
| `minimal-light` | 极简浅色 |

用法：

```sh
./ffx icons                    # 列出预设
./ffx install --icon momo      # 用预设
./ffx install --icon kit --name "Firefox (kit)"
```

`--icon` 取值优先当预设名解析，找不到再当主题图标名（如 `firefox`）使用。
命中的图片会被复制到 `~/.local/share/icons/ffx/<id>.png`，桌面项写绝对路径，
因此 `/mnt/E` 被卸载或这些文件被移动都不影响已安装的图标。

许可证：图标随 Firefox 以 MPL 2.0 发布；Firefox 名称与图标是 Mozilla 的商标，
此处仅用于本机桌面美化。
