"""Build the selected libass font fix, sharing the locked dependency closure."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

import build_deps as deps

ROOT = Path(__file__).resolve().parents[1]

os.environ['PATH'] = str(Path(sys.executable).parent) + os.pathsep + os.environ['PATH']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font-mode', choices=('libass-patch', 'bridge-isolation'), default='libass-patch')
    args = parser.parse_args()
    mode = args.font_mode
    work = ROOT / 'build/deps/ass-font-modes' / mode
    work.mkdir(parents=True, exist_ok=True)
    (work / 'build-report.json').unlink(missing_ok=True)
    (work / 'lib/libass.a').unlink(missing_ok=True)
    lock = json.loads(deps.LOCK_PATH.read_text())
    entry = lock['sources']['libass']
    archive = deps.DOWNLOAD_ROOT / entry['archive_name']
    if digest(archive) != entry['archive_sha256']:
        raise ValueError('locked libass source archive hash mismatch')
    source_root = work / 'source'
    if source_root.exists():
        shutil.rmtree(source_root)
    source_root.mkdir()
    with tarfile.open(archive) as stream:
        stream.extractall(source_root, filter='data')
    children = list(source_root.iterdir())
    if len(children) != 1 or not children[0].is_dir():
        raise ValueError('libass archive must contain one source tree')
    source = children[0]
    patch = ROOT / 'deps/patches/libass-directory-fonts.patch'
    if mode == 'libass-patch':
        subprocess.run(['patch', '--batch', '--fuzz=0', str(source / 'libass/ass_fontselect.c'), str(patch)], check=True)
    # Shared dependencies stay read-only. Meson builds, generated pkg-config,
    # and the libass prefix belong to this mode. Preserve the pinned options.
    deps.BUILD_DIR = work / 'meson'
    deps.PREFIX_ROOT = work / 'prefix'
    deps.LIB_ROOT = work / 'lib'
    deps.INCLUDE_ROOT = work / 'include'
    deps.PKGCONFIG_ROOT = work / 'pkgconfig'
    env = deps._meson_environment(lock)
    build = deps._meson_setup(source, 'libass', lock, env)
    static = deps._install_archive(deps._find_archive(build, 'libass.a'), 'libass.a', env)
    report = {'font_mode': mode, 'source_archive_sha256': digest(archive),
              'patch_sha256': digest(patch) if mode == 'libass-patch' else None,
              'fontselect_sha256': digest(source / 'libass/ass_fontselect.c'),
              'archive_sha256': digest(static)}
    (work / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{mode}: {static}', flush=True)


if __name__ == '__main__':
    main()
