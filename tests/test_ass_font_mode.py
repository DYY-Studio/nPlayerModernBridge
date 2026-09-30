"""Reject stale or mismatched artifacts when choosing a font implementation."""
import hashlib
import json

import pytest

from npabridge import build_bridge


def test_verify_requires_selected_mode_and_matching_artifact(tmp_path, monkeypatch):
    output = tmp_path / 'LibASSBridge.dylib'
    output.write_bytes(b'current-build')
    monkeypatch.setattr(build_bridge, 'OBJECT_ROOT', tmp_path)
    monkeypatch.setattr(build_bridge, 'output_path', lambda _: output)
    (tmp_path / 'libass-font-mode.json').write_text(json.dumps({
        'font_mode': 'bridge-isolation',
        'output_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
    }))
    build_bridge.verify_font_mode('bridge-isolation')
    with pytest.raises(ValueError, match='requested libass-patch'):
        build_bridge.verify_font_mode('libass-patch')
    output.write_bytes(b'replaced-build')
    with pytest.raises(ValueError, match='does not match'):
        build_bridge.verify_font_mode('bridge-isolation')


def test_failed_dependency_build_removes_published_libass(tmp_path, monkeypatch):
    output = tmp_path / 'LibASSBridge.dylib'
    stamp = tmp_path / 'libass-font-mode.json'
    verification = tmp_path / 'libass-verification.json'
    for path in (output, stamp, verification):
        path.write_text('previous-success')
    monkeypatch.setattr(build_bridge, 'OBJECT_ROOT', tmp_path)
    monkeypatch.setattr(build_bridge, 'output_path', lambda _: output)
    monkeypatch.setattr(build_bridge, 'load_closure', lambda _: ((), ()))

    def fail(_):
        raise RuntimeError('source patch failed')

    monkeypatch.setattr(build_bridge, '_run', fail)
    with pytest.raises(RuntimeError, match='source patch failed'):
        build_bridge.build_dylib('libass')
    assert not any(path.exists() for path in (output, stamp, verification))
