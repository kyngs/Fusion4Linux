#!/usr/bin/env python3
"""Install Autodesk Fusion into its own Wine prefix.

Replaces Autodesk's Windows streaming installer: downloads the admin
installer, unpacks its packages into the prefix, and replicates the
installer's post-deploy tasks that Fusion depends on. The installed launcher
(scripts/fusion4linux) carries the runtime workarounds.
"""

import argparse
import concurrent.futures
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import uuid
import xml.etree.ElementTree as ET

INSTALLER_URL = "https://dl.appstreaming.autodesk.com/production/installers/Fusion%20Admin%20Install.exe"
STREAM_TAG = "production"
STREAMER_ROOT_URL = "https://dl.appstreaming.autodesk.com/production/"

# Location ids used as package destinations in the installer's descriptors
LOC_BUILD = "39e8a3d5e919c70b05ef3ebfe9c6bfc8d9ee001e"
LOC_LAUNCHER = "6a0c9611291d45bb9226980209917c3d"
LOC_COMMON = "82533bdd63a14d64a6cd0b10485c044c"

# Fusion ships the Microsoft VC++ runtime next to its binaries. Wine's builtin
# replacements are incomplete (e.g. no std::get_new_handler) and crash the
# cloud browser, so prefer the shipped copies.
NATIVE_DLLS = [
    "concrt140",
    "msvcp140",
    "msvcp140_1",
    "msvcp140_2",
    "msvcp140_atomic_wait",
    "msvcp140_codecvt_ids",
    "vcomp140",
    "vcruntime140",
    "vcruntime140_1",
]

DXVK_DLLS = ["d3d8", "d3d9", "d3d10core", "d3d11", "dxgi"]

URI_SCHEMES = ["adskidmgr", "adsk.idmgr"]

REPO = os.path.dirname(os.path.abspath(__file__))


def data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")


def cache_home():
    return os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")


def wine_env(prefix):
    return dict(os.environ, WINEPREFIX=prefix, WINEDEBUG="-all")


def check_dependencies():
    missing = [tool for tool in ("wine", "wineserver", "7z", "tar", "xz", "curl") if not shutil.which(tool)]
    if missing:
        sys.exit("missing required tools: " + ", ".join(missing))


def download_installer():
    path = os.path.join(cache_home(), "fusion4linux", "Fusion Admin Install.exe")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    part = path + ".part"
    args = ["curl", "-fL", "--retry", "3", "-o", part, INSTALLER_URL]
    if os.path.exists(path):
        # Only fetch when the server has a newer installer than the cached one
        args[1:1] = ["-z", path]
        print("Checking for a newer installer")
    else:
        print(f"Downloading installer to {path}")
    if os.path.exists(part):
        os.remove(part)
    subprocess.run(args, check=True)
    if os.path.exists(part) and os.path.getsize(part) > 0:
        os.replace(part, path)
    elif os.path.exists(part):
        os.remove(part)
    return path


def extract_installer(installer, dest):
    print("Extracting installer")
    subprocess.run(["7z", "x", "-y", "-bso0", "-bsp0", f"-o{dest}", installer], check=True)
    manifests = glob.glob(os.path.join(dest, "_payload", "*", "*", "full.json"))
    if len(manifests) != 1:
        sys.exit(f"unexpected installer layout: expected one full.json, found {len(manifests)}")
    return json.load(open(manifests[0]))


def create_prefix(prefix):
    if os.path.exists(os.path.join(prefix, "system.reg")):
        return
    print(f"Creating Wine prefix {prefix}")
    os.makedirs(prefix, exist_ok=True)
    subprocess.run(["wineboot", "-i"], env=wine_env(prefix), check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["wineserver", "-w"], env=wine_env(prefix), check=True)


def location_map(prefix):
    user = os.environ.get("USER", "user")
    local = os.path.join(prefix, "drive_c", "users", user, "AppData", "Local", "Autodesk")
    webdeploy = os.path.join(local, "webdeploy", STREAM_TAG)
    return {
        # Versioned build directory (Fusion360.exe and most libraries)
        LOC_BUILD: os.path.join(webdeploy, LOC_BUILD),
        # Stable launcher directory
        LOC_LAUNCHER: os.path.join(webdeploy, LOC_LAUNCHER),
        # Shared data (Material Library, Exchange Archives)
        LOC_COMMON: os.path.join(local, "Common"),
    }


def extract_package(tarball, dest):
    os.makedirs(dest, exist_ok=True)
    subprocess.run(["tar", "-xJf", tarball, "-C", dest], check=True)
    return tarball


def deploy_packages(installer_dir, locations, jobs):
    """Unpack each package's tarballs into its destination location."""
    pkg_dir = os.path.join(installer_dir, "_payload", "packages")
    work = []
    for desc_path in sorted(glob.glob(os.path.join(pkg_dir, "*.json"))):
        desc = json.load(open(desc_path))
        dest_id = desc["root"]["destination"]
        if dest_id not in locations:
            sys.exit(f"{os.path.basename(desc_path)}: unknown destination {dest_id}")
        # Packages without a non-patched list ship a tarball named after the descriptor
        tar_ids = desc.get("non-patched") or [os.path.basename(desc_path)[:-len(".json")]]
        for tar_id in tar_ids:
            work.append((os.path.join(pkg_dir, tar_id + ".tar.xz"), locations[dest_id]))

    # Remove the previous build so files dropped by a newer version do not linger
    shutil.rmtree(locations[LOC_BUILD], ignore_errors=True)
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        futures = [pool.submit(extract_package, t, d) for t, d in work]
        for i, f in enumerate(concurrent.futures.as_completed(futures), 1):
            f.result()
            print(f"\rUnpacking packages {i}/{len(work)}", end="", flush=True)
    print()


def configure(build_dir):
    """Replicate the streamer's post-deploy tasks that the app depends on."""
    # ACTIVATE_CONFIG: the shipped server config is a staging copy
    shutil.copy(
        os.path.join(build_dir, "Applications", "Fusion", "Fusion360App", "ApplicationOptions.production.json"),
        os.path.join(build_dir, "Fusion 360.server.config"),
    )

    # PROCESS_TEMPLATE + WRITE_STREAMER_OPTIONS
    streamer_config = os.path.join(build_dir, "Fusion 360.streamer.config")
    tree = ET.parse(streamer_config)
    root = tree.getroot()
    values = {
        "GlobalInstall": "0",
        "StreamerRootUrl": STREAMER_ROOT_URL,
        "InstallStream": STREAM_TAG,
    }
    for opt in list(root):
        key = opt.get("key")
        if key == "DevBuild":
            root.remove(opt)
        elif key in values:
            opt.set("value", values[key])
        else:
            # Production builds have an empty stream tag
            opt.set("value", opt.get("value", "").replace("${streamtag}", ""))
    tree.write(streamer_config, xml_declaration=True, encoding="utf-8")

    # UPDATE_IDSDK_CONFIG: give this install its own SSO server guid
    idsdk_config = os.path.join(build_dir, "Autodesk Identity Manager", "AdskIdentityManagerInstall.config")
    with open(idsdk_config) as f:
        cfg = json.load(f)
    cfg["ssoServer"]["guid"] = str(uuid.uuid4())
    with open(idsdk_config, "w") as f:
        json.dump(cfg, f, indent=4)


def set_dll_overrides(prefix, dlls, mode):
    for dll in dlls:
        subprocess.run(
            ["wine", "reg", "add", r"HKCU\Software\Wine\DllOverrides", "/v", dll, "/d", mode, "/f"],
            env=wine_env(prefix), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )


def install_dxvk(prefix, dxvk_dir):
    """Copy an extracted DXVK release into the prefix (replacing wined3d)."""
    system = os.path.join(prefix, "drive_c", "windows")
    for arch, sysdir in (("x64", "system32"), ("x32", "syswow64")):
        for dll in DXVK_DLLS:
            shutil.copy(os.path.join(dxvk_dir, arch, dll + ".dll"), os.path.join(system, sysdir, dll + ".dll"))


def install_webview2(prefix, build_dir):
    """INSTALL_WEBVIEW2: the Identity Manager needs WebView2 for sign-in."""
    if os.path.isdir(os.path.join(prefix, "drive_c", "Program Files (x86)", "Microsoft", "EdgeWebView", "Application")):
        return
    print("Installing WebView2 runtime")
    subprocess.run(
        ["wine", os.path.join(build_dir, "MicrosoftEdgeWebView2RuntimeInstallerX64.exe"), "/silent", "/install"],
        env=wine_env(prefix), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def install_scripts(home):
    bindir = os.path.join(home, "bin")
    os.makedirs(bindir, exist_ok=True)
    for name in ("fusion4linux", "fusion4linux-uri-handler"):
        dest = os.path.join(bindir, name)
        shutil.copy(os.path.join(REPO, "scripts", name), dest)
        os.chmod(dest, 0o755)
    return bindir


def install_icon(build_dir, home):
    """Convert Fusion's .ico to a PNG for the desktop entry, if ImageMagick is available."""
    magick = shutil.which("magick")
    if not magick:
        return None
    ico = os.path.join(build_dir, "Applications", "Fusion", "Fusion360App", "Fusion360.ico")
    png = os.path.join(home, "fusion360.png")
    # Pick the largest frame of the icon
    result = subprocess.run([magick, ico + "[-1]", png], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return png if result.returncode == 0 else None


def install_desktop_entries(bindir, icon):
    apps = os.path.join(data_home(), "applications")
    os.makedirs(apps, exist_ok=True)
    launcher = os.path.join(bindir, "fusion4linux")
    with open(os.path.join(apps, "fusion4linux.desktop"), "w") as f:
        f.write("[Desktop Entry]\n"
                "Type=Application\n"
                "Name=Autodesk Fusion\n"
                f'Exec="{launcher}"\n'
                + (f"Icon={icon}\n" if icon else "")
                + "Categories=Graphics;Engineering;\n"
                "StartupWMClass=fusion360.exe\n"
                "Terminal=false\n")

    # The browser hands the sign-in result back through these URL schemes
    handler = os.path.join(bindir, "fusion4linux-uri-handler")
    with open(os.path.join(apps, "fusion4linux-idmgr.desktop"), "w") as f:
        f.write("[Desktop Entry]\n"
                "Type=Application\n"
                "Name=Autodesk Identity Manager (Fusion4Linux)\n"
                f'Exec="{handler}" %u\n'
                "MimeType=" + "".join(f"x-scheme-handler/{s};" for s in URI_SCHEMES) + "\n"
                "NoDisplay=true\n"
                "Terminal=false\n")

    if shutil.which("update-desktop-database"):
        subprocess.run(["update-desktop-database", apps], stderr=subprocess.DEVNULL)
    if shutil.which("xdg-mime"):
        for scheme in URI_SCHEMES:
            subprocess.run(["xdg-mime", "default", "fusion4linux-idmgr.desktop", f"x-scheme-handler/{scheme}"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--installer", help="use this Fusion Admin Install.exe instead of downloading it")
    ap.add_argument("--home", default=os.path.join(data_home(), "fusion4linux"),
                    help="installation directory (default: %(default)s)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--dxvk", help="extracted DXVK release directory (containing x64/ and x32/). "
                    "Currently breaks the embedded browsers: the launcher disables their GPU use, "
                    "and DXVK cannot create the WARP software device they then fall back to")
    ap.add_argument("--no-desktop", action="store_true", help="skip the menu entry and sign-in URL handler")
    args = ap.parse_args()

    check_dependencies()
    home = os.path.abspath(args.home)
    prefix = os.path.join(home, "prefix")
    installer = os.path.abspath(args.installer) if args.installer else download_installer()

    create_prefix(prefix)
    # Nothing may run in the prefix while its files are replaced
    subprocess.run(["wineserver", "-k"], env=wine_env(prefix), stderr=subprocess.DEVNULL)

    os.makedirs(os.path.join(cache_home(), "fusion4linux"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(cache_home(), "fusion4linux")) as tmp:
        manifest = extract_installer(installer, tmp)
        print(f"Fusion build {manifest['build-version']}")
        locations = location_map(prefix)
        deploy_packages(tmp, locations, args.jobs)

    build_dir = locations[LOC_BUILD]
    configure(build_dir)

    set_dll_overrides(prefix, NATIVE_DLLS, "native,builtin")
    if args.dxvk:
        install_dxvk(prefix, args.dxvk)
        set_dll_overrides(prefix, DXVK_DLLS, "native,builtin")
    else:
        set_dll_overrides(prefix, DXVK_DLLS, "builtin")

    install_webview2(prefix, build_dir)

    bindir = install_scripts(home)
    if not args.no_desktop:
        install_desktop_entries(bindir, install_icon(build_dir, home))

    print(f"Installed. Start Fusion with {os.path.join(bindir, 'fusion4linux')}"
          + ("" if args.no_desktop else " or from your application menu"))


if __name__ == "__main__":
    main()
