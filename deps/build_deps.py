"""Cross-compile the static iOS 13 libass dependency closure.

Sources come from officially released tarballs (or a pinned commit when
upstream publishes no release), each verified against the SHA-256 in
deps/sources.lock.json. Only static archives and headers are produced.
"""

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / "deps"
LOCK_PATH = DEPS / "sources.lock.json"
CROSS_FILE = DEPS / "ios-arm64.cross"
NATIVE_FILE = DEPS / "macos-arm64.native"
BUILD_ROOT = ROOT / "build" / "deps"
DOWNLOAD_ROOT = BUILD_ROOT / "downloads"
SOURCE_ROOT = BUILD_ROOT / "sources"
BUILD_DIR = BUILD_ROOT / "build"
PREFIX_ROOT = BUILD_ROOT / "prefix"
LIB_ROOT = BUILD_ROOT / "lib"
INCLUDE_ROOT = BUILD_ROOT / "include"
PKGCONFIG_ROOT = BUILD_ROOT / "pkgconfig"
CLOSURE_PATH = BUILD_ROOT / "libass-closure.txt"
VERIFICATION_PATH = BUILD_ROOT / "verification.json"
TARGET = "arm64-apple-ios13.0"
FORBIDDEN_PATH_PREFIXES = ("/usr/local/", "/opt/homebrew/", "/Users/")
SYSTEM_PATH = os.pathsep.join(("/usr/bin", "/bin", "/usr/sbin", "/sbin"))
SAFE_ENV_KEYS = {
    "HOME",
    "LANG",
    "LC_ALL",
    "LC_CTYPE",
    "LOGNAME",
    "OLDPWD",
    "PWD",
    "SHELL",
    "TERM",
    "TMPDIR",
    "TZ",
    "USER",
}
ENTRY_FIELDS = ("version", "url", "archive_sha256", "archive_name", "output_archive")
ARCHIVE_EXTENSIONS = {".tar", ".gz", ".xz", ".bz2", ".bz3", ".lz"}
ALLOWED_ARCHIVE_MEMBERS = {"__.SYMDEF", "__.SYMDEF SORTED"}


def isolated_environment(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = {key: os.environ[key] for key in SAFE_ENV_KEYS if key in os.environ}
    env["PATH"] = SYSTEM_PATH
    if extra:
        env.update(extra)
    return env


def run(
    command: list[object],
    env: dict[str, str],
    cwd: Path | None = None,
) -> None:
    subprocess.run(
        [str(part) for part in command],
        cwd=str(cwd) if cwd is not None else None,
        env=env,
        check=True,
    )


def sanitize_ffmpeg_configuration(config: Path, root: Path = ROOT) -> None:
    lines = config.read_text(encoding="utf-8").splitlines(keepends=True)
    prefix = "#define FFMPEG_CONFIGURATION "
    matches = [index for index, line in enumerate(lines) if line.startswith(prefix)]
    if len(matches) != 1:
        raise ValueError(f"FFmpeg config has {len(matches)} configuration macros: {config}")
    index = matches[0]
    checkout = str(root)
    if checkout not in lines[index]:
        raise ValueError(f"FFmpeg configuration does not contain checkout path: {config}")
    lines[index] = lines[index].replace(checkout, ".")
    config.write_text("".join(lines), encoding="utf-8")


def _xcrun(name: str, sdk: str) -> str:
    result = subprocess.run(
        ["/usr/bin/xcrun", "--sdk", sdk, "--find", name],
        env=isolated_environment(),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        raise RuntimeError(f"unable to find {sdk} tool {name}: {result.stdout.strip()}")
    return result.stdout.strip()


def sdk_path() -> str:
    result = subprocess.run(
        ["/usr/bin/xcrun", "--sdk", "iphoneos", "--show-sdk-path"],
        env=isolated_environment(),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        raise RuntimeError(f"unable to find the iOS SDK: {result.stdout.strip()}")
    return result.stdout.strip()


def host_tool(name: str) -> str:
    tool = shutil.which(name)
    if not tool:
        raise RuntimeError(f"host tool is required: {name}")
    return tool


def _sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or len(value) != 64:
        raise ValueError(f"dependency lock hash is invalid: {label}")
    try:
        int(value, 16)
    except ValueError as error:
        raise ValueError(f"dependency lock hash is invalid: {label}") from error
    return value


def load_lock(path: Path = LOCK_PATH) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema") != 1:
        raise ValueError("dependency lock schema is invalid")
    if data.get("target") != TARGET:
        raise ValueError("dependency lock target is not iOS 13 arm64")
    sources = data.get("sources")
    if not isinstance(sources, dict):
        raise ValueError("dependency lock has no sources")
    build_order = tuple(data.get("build_order", ()))
    if set(build_order) != set(sources):
        raise ValueError("dependency lock build order does not match its sources")
    for name, entry in sources.items():
        if name not in build_order:
            raise ValueError(f"dependency lock source is not in the build order: {name}")
        if not isinstance(entry, dict):
            raise ValueError(f"dependency lock entry is invalid: {name}")
        missing = [field for field in ENTRY_FIELDS if field not in entry]
        if missing:
            raise ValueError(f"dependency lock entry is incomplete: {name}: {missing}")
        if not entry["url"].startswith("https://"):
            raise ValueError(f"dependency source URL is not HTTPS: {name}")
        _sha256(entry["archive_sha256"], name)
        if Path(entry["archive_name"]).name != entry["archive_name"]:
            raise ValueError(f"dependency archive name is unsafe: {name}")
        if Path(entry["archive_name"]).suffix not in ARCHIVE_EXTENSIONS:
            raise ValueError(f"dependency archive type is unsupported: {name}")
        if not entry["output_archive"].startswith("build/deps/lib/"):
            raise ValueError(f"dependency output path is unsafe: {name}")
        if not isinstance(entry["build_options"], dict):
            raise ValueError(f"dependency build options are invalid: {name}")
        for dependency in entry.get("dependencies", {}):
            if dependency not in sources:
                raise ValueError(f"dependency link is invalid: {name}:{dependency}")
    for project in data.get("system_dependencies", {}):
        if project not in sources:
            raise ValueError(f"system dependency project is unknown: {project}")
    return data


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_source(name: str, lock: dict[str, Any]) -> Path:
    entry = lock["sources"][name]
    archive = DOWNLOAD_ROOT / entry["archive_name"]
    if archive.is_file():
        actual = sha256_file(archive)
        if actual == entry["archive_sha256"]:
            print(f"reusing {archive}", flush=True)
            return archive
        raise ValueError(f"{name} cached archive hash mismatch: {actual}")
    DOWNLOAD_ROOT.mkdir(parents=True, exist_ok=True)
    temporary = archive.with_name(archive.name + ".part")
    with urllib.request.urlopen(entry["url"]) as response, temporary.open("wb") as stream:
        shutil.copyfileobj(response, stream)
    os.replace(temporary, archive)
    actual = sha256_file(archive)
    if actual != entry["archive_sha256"]:
        archive.unlink()
        raise ValueError(f"{name} source archive hash mismatch: {actual}")
    return archive


def extract_source(name: str, lock: dict[str, Any]) -> Path:
    tree = SOURCE_ROOT / name / "tree"
    if tree.is_dir():
        return tree
    archive = fetch_source(name, lock)
    destination = SOURCE_ROOT / name
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    with tarfile.open(archive, "r:*") as stream:
        stream.extractall(destination, filter="data")
    children = [child for child in destination.iterdir()]
    if len(children) != 1 or not children[0].is_dir():
        raise ValueError(f"source archive does not contain one directory: {archive.name}")
    children[0].rename(tree)
    return tree


def _apply_source_patches(source: Path, lock: dict[str, Any], name: str) -> Path:
    patches = lock.get("source_patches", {}).get(name, [])
    if not patches:
        return source
    target = BUILD_DIR / f"{name}-src"
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(source, target, symlinks=True)
    for patch in patches:
        path = target / patch["path"]
        content = path.read_text(encoding="utf-8")
        if not patch["old"] or content.count(patch["old"]) != 1:
            raise ValueError(f"source patch does not apply exactly: {name}:{patch['path']}")
        path.write_text(content.replace(patch["old"], patch["new"], 1), encoding="utf-8")
    return target


def _write_pkg_config(lock: dict[str, Any], sdk: str) -> None:
    PKGCONFIG_ROOT.mkdir(parents=True, exist_ok=True)
    prefix = str(BUILD_ROOT)
    include_roots = {
        "freetype": "${prefix}/include/freetype2",
        "harfbuzz": "${prefix}/include/harfbuzz",
        "fribidi": "${prefix}/include/fribidi",
    }
    for name, entry in lock["sources"].items():
        archive = Path(entry["output_archive"]).name
        stem = archive[3:-2]
        package = entry.get("package", name)
        version = entry.get("pkgconfig_version", entry["version"])
        dependencies = " ".join(entry.get("dependencies", {}).values())
        (PKGCONFIG_ROOT / f"{package}.pc").write_text(
            f"""prefix={prefix}
libdir=${{prefix}}/lib
includedir={include_roots.get(name, "${prefix}/include")}
Name: {package}
Description: locked {name}
Version: {version}
Requires: {dependencies}
Libs: -L${{libdir}} -l{stem}
Cflags: -I${{includedir}}
""",
            encoding="utf-8",
        )
    zlib = lock["system_dependencies"]["freetype"]["zlib"]
    (PKGCONFIG_ROOT / "zlib.pc").write_text(
        f"""prefix={sdk}
libdir=${{prefix}}/usr/lib
includedir=${{prefix}}/usr/include
Name: zlib
Description: Apple iOS SDK zlib
Version: {zlib['version']}
Libs: -L${{libdir}} -lz
Cflags: -I${{includedir}}
""",
        encoding="utf-8",
    )


def _build_environment(lock: dict[str, Any]) -> dict[str, str]:
    """Environment for autoconf (Expat) and for plain ar/ranlib work."""

    sdk = sdk_path()
    _write_pkg_config(lock, sdk)
    for directory in (BUILD_DIR, PREFIX_ROOT, LIB_ROOT, INCLUDE_ROOT):
        directory.mkdir(parents=True, exist_ok=True)
    target_common = [
        "-target",
        TARGET,
        "-isysroot",
        sdk,
        "-arch",
        "arm64",
        "-miphoneos-version-min=13.0",
        "-g0",
        "-ffile-prefix-map=" + str(ROOT) + "=.",
        "-fdebug-prefix-map=" + str(ROOT) + "=.",
    ]
    pkg_config = host_tool("pkg-config")
    venv_bin = str(Path(host_tool("meson")).parent)
    return isolated_environment(
        {
            "CC": _xcrun("clang", "iphoneos"),
            "CXX": _xcrun("clang++", "iphoneos"),
            "AR": _xcrun("ar", "iphoneos"),
            "RANLIB": _xcrun("ranlib", "iphoneos"),
            "STRIP": _xcrun("strip", "iphoneos"),
            "PKG_CONFIG": pkg_config,
            "PKG_CONFIG_LIBDIR": str(PKGCONFIG_ROOT),
            "PKG_CONFIG_PATH": "",
            "CFLAGS": shlex.join(target_common),
            "CXXFLAGS": shlex.join(target_common + ["-stdlib=libc++"]),
            "LDFLAGS": shlex.join(target_common),
            "NINJA": host_tool("ninja"),
            "MESON": host_tool("meson"),
            "PATH": os.pathsep.join([venv_bin, str(Path(pkg_config).parent), SYSTEM_PATH]),
            "SDKROOT": "iphoneos",
            "IPHONEOS_DEPLOYMENT_TARGET": "13.0",
            "ARCHS": "arm64",
            "PYTHON": sys.executable,
        }
    )


def _meson_environment(lock: dict[str, Any]) -> dict[str, str]:
    """Environment for Meson.

    The cross and native files carry every compiler, flag and target
    detail, so no SDKROOT, CFLAGS or CC leaks into the native compiler.
    """

    _write_pkg_config(lock, sdk_path())
    for directory in (BUILD_DIR, PREFIX_ROOT, LIB_ROOT, INCLUDE_ROOT):
        directory.mkdir(parents=True, exist_ok=True)
    pkg_config = host_tool("pkg-config")
    venv_bin = str(Path(host_tool("meson")).parent)
    return isolated_environment(
        {
            "PKG_CONFIG": pkg_config,
            "PKG_CONFIG_LIBDIR": str(PKGCONFIG_ROOT),
            "PKG_CONFIG_PATH": "",
            "NINJA": host_tool("ninja"),
            "MESON": host_tool("meson"),
            "PATH": os.pathsep.join([venv_bin, str(Path(pkg_config).parent), SYSTEM_PATH]),
            "PYTHON": sys.executable,
        }
    )


def _copy_headers(source: Path, destination: Path, base: Path | None = None) -> int:
    root = base or source
    count = 0
    for path in sorted(source.rglob("*.h")):
        if not path.is_file() or path.is_symlink():
            continue
        target = destination / path.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        count += 1
    return count


def _find_archive(build: Path, filename: str) -> Path:
    matches = sorted(path for path in build.rglob(filename) if path.is_file())
    if len(matches) != 1:
        raise ValueError(f"Meson build produced {len(matches)} copies of {filename}")
    return matches[0]


def _install_archive(source: Path, filename: str, env: dict[str, str]) -> Path:
    LIB_ROOT.mkdir(parents=True, exist_ok=True)
    target = LIB_ROOT / filename
    shutil.copy2(source, target)
    run([_xcrun("ranlib", "iphoneos"), target], env)
    return target


def _meson_setup(source: Path, name: str, lock: dict[str, Any], env: dict[str, str]) -> Path:
    build = BUILD_DIR / name
    if build.exists():
        shutil.rmtree(build)
    options = dict(lock["sources"][name]["build_options"])
    options.update(
        {
            "prefix": str(PREFIX_ROOT / name),
            "libdir": "lib",
            "includedir": "include",
        }
    )
    args = [
        host_tool("meson"),
        "setup",
        str(build),
        str(source),
        "--cross-file",
        str(CROSS_FILE),
        "--native-file",
        str(NATIVE_FILE),
        "--backend=ninja",
        "--buildtype=release",
        "--wrap-mode=nodownload",
    ]
    for key, value in options.items():
        if isinstance(value, bool):
            rendered = "true" if value else "false"
        else:
            rendered = str(value)
        args.append(f"-D{key}={rendered}")
    run(args, env)
    run([host_tool("ninja"), "-C", str(build)], env)
    return build


def build_expat(source: Path, lock: dict[str, Any], env: dict[str, str]) -> None:
    options = lock["sources"]["expat"]["build_options"]
    command = [
        "./configure",
        "--host=arm64-apple-darwin",
        "--prefix=" + str(PREFIX_ROOT / "expat"),
    ]
    if options["shared"] is False:
        command.append("--disable-shared")
    if options["tools"] is False:
        command.append("--without-xmlwf")
    if options["examples"] is False:
        command.append("--without-examples")
    if options["tests"] is False:
        command.append("--without-tests")
    if options["docs"] is False:
        command.append("--without-docbook")
    run(command, env, source)
    run(["/usr/bin/make", "-j2"], env, source)
    run(["/usr/bin/make", "install"], env, source)
    prefix = PREFIX_ROOT / "expat"
    _install_archive(prefix / "lib" / "libexpat.a", "libexpat.a", env)
    INCLUDE_ROOT.mkdir(parents=True, exist_ok=True)
    for header in ("expat.h", "expat_external.h"):
        shutil.copy2(prefix / "include" / header, INCLUDE_ROOT / header)


def build_freetype(source: Path, lock: dict[str, Any], env: dict[str, str]) -> None:
    build = _meson_setup(source, "freetype", lock, env)
    _install_archive(_find_archive(build, "libfreetype.a"), "libfreetype.a", env)
    destination = INCLUDE_ROOT / "freetype2"
    _copy_headers(source / "include", destination, source / "include")
    for generated in ("ftconfig.h", "ftmodule.h", "ftoption.h"):
        path = build / generated
        if path.is_file():
            target = destination / "freetype" / "config" / generated
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def build_harfbuzz(source: Path, lock: dict[str, Any], env: dict[str, str]) -> None:
    build = _meson_setup(source, "harfbuzz", lock, env)
    _install_archive(_find_archive(build, "libharfbuzz.a"), "libharfbuzz.a", env)
    _copy_headers(source / "src", INCLUDE_ROOT / "harfbuzz", source / "src")


def build_fribidi(source: Path, lock: dict[str, Any], env: dict[str, str]) -> None:
    build = _meson_setup(source, "fribidi", lock, env)
    _install_archive(_find_archive(build, "libfribidi.a"), "libfribidi.a", env)
    destination = INCLUDE_ROOT / "fribidi"
    for root in ("lib", "gen.tab"):
        _copy_headers(source / root, destination, source / root)
    for generated in (
        build / "lib" / "fribidi-config.h",
        build / "gen.tab" / "fribidi-unicode-version.h",
    ):
        if generated.is_file():
            shutil.copy2(generated, destination / generated.name)


def build_fontconfig(source: Path, lock: dict[str, Any], env: dict[str, str]) -> None:
    build = _meson_setup(source, "fontconfig", lock, env)
    _install_archive(_find_archive(build, "libfontconfig.a"), "libfontconfig.a", env)
    destination = INCLUDE_ROOT / "fontconfig"
    _copy_headers(source / "fontconfig", destination, source / "fontconfig")
    _copy_headers(build / "fontconfig", destination, build / "fontconfig")


def build_libass(source: Path, lock: dict[str, Any], env: dict[str, str]) -> None:
    build = _meson_setup(source, "libass", lock, env)
    _install_archive(_find_archive(build, "libass.a"), "libass.a", env)
    _copy_headers(source / "libass", INCLUDE_ROOT / "ass", source / "libass")
    write_libass_closure(lock)


BUILDERS = {
    "expat": build_expat,
    "freetype": build_freetype,
    "harfbuzz": build_harfbuzz,
    "fribidi": build_fribidi,
    "fontconfig": build_fontconfig,
    "libass": build_libass,
}


def dependency_closure(lock: dict[str, Any], root: str) -> list[str]:
    ordered: list[str] = []
    visiting: set[str] = set()

    def visit(name: str) -> None:
        if name in visiting:
            raise ValueError(f"dependency cycle detected at {name}")
        if name in ordered:
            return
        visiting.add(name)
        ordered.append(name)
        for dependency in lock["sources"][name].get("dependencies", {}):
            visit(dependency)
        visiting.remove(name)

    visit(root)
    return ordered


def system_link_args(lock: dict[str, Any], root: str) -> list[str]:
    arguments: list[str] = []
    for project in dependency_closure(lock, root):
        for specification in lock.get("system_dependencies", {}).get(project, {}).values():
            for argument in specification["link_args"]:
                if argument not in arguments:
                    arguments.append(argument)
    return arguments


def write_libass_closure(lock: dict[str, Any]) -> None:
    archives = [
        Path(lock["sources"][name]["output_archive"]).name
        for name in dependency_closure(lock, "libass")
    ]
    CLOSURE_PATH.write_text(
        "project_archives="
        + " ".join(archives)
        + "\nsystem_link_args="
        + " ".join(system_link_args(lock, "libass"))
        + "\n",
        encoding="utf-8",
    )


def validate_cross_file(path: Path = CROSS_FILE) -> None:
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    if not parser.read(path, encoding="utf-8"):
        raise ValueError(f"cross file cannot be read: {path}")
    host = parser["host_machine"]
    properties = parser["properties"]

    def value(section: configparser.SectionProxy, key: str) -> str:
        return section[key].strip().strip("'\"")

    if (
        value(host, "system"),
        value(host, "cpu_family"),
        value(host, "cpu"),
        value(host, "endian"),
    ) != ("darwin", "aarch64", "arm64", "little"):
        raise ValueError("cross file host machine is not iOS arm64")
    if value(properties, "target") != TARGET:
        raise ValueError("cross file target is not iOS 13 arm64")
    if value(properties, "needs_exe_wrapper").lower() != "true":
        raise ValueError("cross file must declare needs_exe_wrapper")


def _validate_archive(archive: Path, env: dict[str, str]) -> dict[str, Any]:
    import lief

    if not archive.is_file() or archive.is_symlink():
        raise ValueError(f"dependency output is not a static archive: {archive}")
    if archive.read_bytes()[:8] == b"!<thin>\n":
        raise ValueError(f"dependency output is a thin archive: {archive}")
    listing = subprocess.run(
        [_xcrun("ar", "iphoneos"), "-t", str(archive)],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        check=True,
    ).stdout
    members = [line.strip() for line in listing.splitlines() if line.strip()]
    objects = [member for member in members if member.endswith(".o")]
    unexpected = set(members) - set(objects) - ALLOWED_ARCHIVE_MEMBERS
    if unexpected:
        raise ValueError(f"dependency archive has unexpected members: {sorted(unexpected)}")
    if not objects:
        raise ValueError(f"dependency archive has no objects: {archive}")
    with tempfile.TemporaryDirectory(prefix="npa-archive-") as temporary:
        directory = Path(temporary)
        for member in objects:
            run([_xcrun("ar", "iphoneos"), "-x", archive, member], env, directory)
            object_path = directory / member
            parsed = lief.MachO.parse(str(object_path))
            binary = list(parsed) if parsed is not None else []
            if len(binary) != 1:
                raise ValueError(f"archive member is not one Mach-O object: {member}")
            header = binary[0].header
            build_version = binary[0].build_version
            if header.cpu_type != lief.MachO.Header.CPU_TYPE.ARM64:
                raise ValueError(f"archive member is not arm64: {member}")
            if build_version is None:
                raise ValueError(f"archive member has no LC_BUILD_VERSION: {member}")
            platform = str(build_version.platform).rsplit(".", 1)[-1].upper()
            minos = tuple(int(part) for part in build_version.minos)[:2]
            if platform != "IOS" or minos != (13, 0):
                raise ValueError(
                    f"archive member target is not iOS 13 arm64: {member} {platform} {minos}"
                )
    return {
        "path": str(archive),
        "sha256": sha256_file(archive),
        "object_count": len(objects),
    }


def verify_closure(lock: dict[str, Any] | None = None) -> dict[str, Any]:
    lock = load_lock() if lock is None else lock
    validate_cross_file()
    env = _meson_environment(lock)
    archives: dict[str, Any] = {}
    paths: list[Path] = []
    for name in lock["build_order"]:
        archive = LIB_ROOT / Path(lock["sources"][name]["output_archive"]).name
        archives[name] = _validate_archive(archive, env)
        paths.append(archive)
    paths.extend(path for path in INCLUDE_ROOT.rglob("*") if path.is_file())
    expected = [
        Path(lock["sources"][name]["output_archive"]).name for name in lock["build_order"]
    ]
    present = sorted(path.name for path in LIB_ROOT.iterdir() if path.is_file())
    if present != sorted(expected):
        raise ValueError(f"dependency archive set mismatch: {present}")
    violations = [
        f"{path}: {prefix}"
        for path in paths
        for prefix in FORBIDDEN_PATH_PREFIXES
        if prefix.encode() in path.read_bytes()
    ]
    if violations:
        raise ValueError("forbidden host path in dependency output: " + "; ".join(violations))
    write_libass_closure(lock)
    report = {
        "target": TARGET,
        "cross_file": str(CROSS_FILE),
        "archives": archives,
        "include_root": str(INCLUDE_ROOT),
        "lib_root": str(LIB_ROOT),
        "system_link_args": system_link_args(lock, "libass"),
        "path_hygiene": "passed",
    }
    VERIFICATION_PATH.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return report


def _reset_build_tree() -> None:
    for name in ("sources", "build", "prefix", "lib", "include", "pkgconfig"):
        path = BUILD_ROOT / name
        if path.exists():
            shutil.rmtree(path)
    BUILD_ROOT.mkdir(parents=True, exist_ok=True)


def build_all() -> dict[str, Any]:
    lock = load_lock()
    validate_cross_file()
    _reset_build_tree()
    env = _build_environment(lock)
    meson_env = _meson_environment(lock)
    for name in lock["build_order"]:
        print(f"building {name}", flush=True)
        source = _apply_source_patches(extract_source(name, lock), lock, name)
        BUILDERS[name](source, lock, env if name == "expat" else meson_env)
    return verify_closure(lock)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify-only", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        report = verify_closure() if arguments.verify_only else build_all()
        print(json.dumps(report, indent=2, sort_keys=True))
    except Exception as error:
        print(f"dependency build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
