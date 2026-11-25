# Fusion4Linux

Run Autodesk Fusion on Linux with plain Wine.

Instead of running Autodesk's Windows installer, `install.py` unpacks the
packages from the official admin installer straight into a dedicated Wine
prefix and replicates the installer's setup steps that Fusion depends on. The
installed launcher applies the runtime workarounds Fusion needs under Wine.

## Status

Tested with Fusion 2706.1.17 on Wine 11.21 (NVIDIA RTX 5080).

- Works: installation, sign-in, data panel and other embedded web views,
  rendering designs in the 3D canvas (with DXVK and the "DirectX 11" or
  "Auto-select" graphics driver)
- Not tested yet: AMD and Intel GPUs, Wine's Wayland driver (with Wine 11.21
  the canvas goes blank there)

## Requirements

- Wine (64-bit, with WoW64), running on X11 or XWayland
- A Vulkan capable GPU and driver (for DXVK)
- `7z` (p7zip / 7-Zip), `tar`, `xz`, `curl`, Python 3
- `gcc` and the Vulkan and XCB development headers (`vulkan-headers`,
  `libxcb`), to build the layer that keeps the 3D canvas up to date. Without
  them Fusion still works, but hover highlights may lag one frame behind
- Optional: ImageMagick (`magick`) for the menu icon

## Install

```sh
./install.py
```

This downloads the Fusion admin installer (about 1.5 GB) and DXVK to
`~/.cache/fusion4linux/` and installs Fusion (about 7.5 GB) to
`~/.local/share/fusion4linux/`. It also adds an "Autodesk Fusion" menu entry
and registers the URL handler that completes browser sign-in.

Options:

- `--installer PATH` use an already downloaded `Fusion Admin Install.exe`
- `--home DIR` install somewhere else
- `--dxvk DIR` use an already extracted DXVK release instead of downloading it
- `--no-dxvk` use Wine's own Direct3D (designs do not render in the canvas)
- `--no-desktop` skip the menu entry and sign-in URL handler

Run `install.py` again to update: it only downloads the installer if Autodesk
published a newer one, then replaces the installed build. Your Fusion data and
sign-in are kept.

## Run

Start "Autodesk Fusion" from the application menu, or:

```sh
~/.local/share/fusion4linux/bin/fusion4linux
```

Sign-in opens in your web browser. When the browser asks how to open the
link, choose "Autodesk Identity Manager (Fusion4Linux)".

## Troubleshooting

- "Sign in Failure" right after the very first sign-in: close Fusion and start
  it again. Fusion requests the profile slightly before it is saved.
- Fusion's own logs are in
  `prefix/drive_c/users/$USER/AppData/Local/Autodesk/Neutron Platform/logs/`
  and the sign-in logs in `.../AppData/Local/Autodesk/Identity Services/Log/`.

## How it works

`install.py`:

- Unpacks each package of the installer into its destination, the same layout
  the Windows installer produces under
  `%LOCALAPPDATA%\Autodesk\webdeploy\production\`
- Activates the production configuration (the installer ships a staging one
  and rewrites it during installation)
- Prefers the Microsoft VC++ runtime and HLSL shader compiler shipped with
  Fusion over Wine's builtin ones. Wine's runtime lacks functions Fusion uses,
  and its shader compiler fails on the canvas shaders, so designs would not
  render
- Installs DXVK, which renders the 3D canvas through Vulkan
- Installs the bundled WebView2 runtime, needed by the sign-in component
- Builds a small Vulkan layer (`layer/present_sync.c`). Wine draws child
  windows such as the 3D canvas offscreen and copies them to the screen right
  after each present, before the new frame has actually arrived, so the canvas
  often shows the previous frame (hover highlights lag one step behind). The
  layer waits until the X server reports the window changed before letting
  Wine copy it

The launcher (`scripts/fusion4linux`):

- Disables the Chromium sandbox of the embedded web views. Under Wine their
  renderer processes otherwise never start and every panel stays blank
- Disables GPU use in the embedded web views. It relies on Direct3D textures
  shared between processes, which Wine does not fully implement
- Starts the Identity Manager before Fusion and waits until it is ready.
  Otherwise Fusion can start two of them, which then block each other and
  sign-in fails with a misleading firewall error
- Stops leftover background processes (Identity Manager and others) before
  and after each run, so a stale one cannot break the next sign-in
- Loads the Vulkan layer built by `install.py`
- Keeps Fusion's "Unsupported OS" warning from coming due. Fusion detects
  Wine and repeats the warning periodically; the launcher resets the time it
  was last shown (it still appears once, on the very first start)

## Uninstall

```sh
rm -rf ~/.local/share/fusion4linux ~/.cache/fusion4linux
rm ~/.local/share/applications/fusion4linux.desktop ~/.local/share/applications/fusion4linux-idmgr.desktop
```
