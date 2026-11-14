# About this fork

This is a personal fork of [deskflow/deskflow](https://github.com/deskflow/deskflow)
that makes **clipboard sharing work on a Deskflow server running on Hyprland**.
Everything else is upstream Deskflow; clients (macOS, Windows, other Linux
machines) keep using the official builds.

## Why it exists

Deskflow 1.27 on Wayland reaches the clipboard only through the XDG Clipboard
portal. `xdg-desktop-portal-hyprland` does not implement that portal, so on a
Hyprland server clipboard sharing is silently disabled in both directions,
even with `clipboardSharing = true`.

Upstream decided to support portals only and closed the fallbacks:

- [#10315](https://github.com/deskflow/deskflow/issues/10315) — closed as not planned
- [#10337](https://github.com/deskflow/deskflow/pull/10337) — wl-clipboard bridge, closed without merging
- [#10135](https://github.com/deskflow/deskflow/issues/10135) — wl-clipboard support will not come back

The proper long-term fix is a Clipboard portal in xdg-desktop-portal-hyprland
([hyprwm/xdg-desktop-portal-hyprland#252](https://github.com/hyprwm/xdg-desktop-portal-hyprland/issues/252)).
Once that lands, this fork is no longer needed.

## What is different

| Change | Commit | Notes |
|---|---|---|
| Clipboard fallback through `ext-data-control-v1` | `platform: wayland data-control clipboard fallback…` | Patch by [gogetwoo](https://github.com/gogetwoo/deskflow-hyprland-clipboard). Used only when the portal session has no clipboard. Talks Wayland directly on its own thread: no polling, no `wl-copy`/`wl-paste` processes, no network listeners. |
| Thread-safety fixes for the fallback | `fix(data-control): …` | Pastes are served from a snapshot taken on the thread that owns the clipboard cache (the original read the shared cache from the Wayland thread). Queued tasks no longer touch a device the compositor already finished. `m_stop` is atomic. |
| `BUILD_GUI` CMake option | same commit | `-DBUILD_GUI=OFF` builds only `deskflow-core`, without the Qt GUI. |
| Arch package | `packaging/arch/PKGBUILD` | `deskflow-hyprland-git`: core only, `provides`/`conflicts` `deskflow`. |
| Weekly rebase on upstream | `.github/workflows/fork-sync.yml` | See below. |

The second patch from gogetwoo (portal barriers on exterior edges) is not included.

When the fallback is active, the server logs:

```
INFO: using wayland data-control for clipboard sharing
```

## Keeping up with upstream

`.github/workflows/fork-sync.yml` runs every Monday (and on demand):

1. rebases the fork commits on `upstream/master`,
2. builds `deskflow-core` with `-DBUILD_GUI=OFF` in an Arch container,
3. checks that the data-control fallback is compiled in,
4. on success force-pushes the rebased branch `hyprland-clipboard` (the default branch),
5. on a conflict or build failure opens an issue in this repository and changes nothing.

`hyprland-clipboard` is therefore a patch queue on top of upstream: its history
is rewritten when upstream moves.

## Install (Arch, Hyprland server)

```sh
cd packaging/arch
makepkg -si
systemctl --user restart deskflow-server   # or however deskflow-core server is started
```

The package conflicts with `deskflow`/`deskflow-git`, so pacman will not
overwrite it during `-Syu`. Rebuild it after the workflow updates the branch.

## License

GPL-2.0, same as Deskflow. The fallback patch keeps its original author.
