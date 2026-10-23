import subprocess
from pathlib import Path

import pytest
from npabridge.renderer_bindings import generate_header

ROOT = Path(__file__).resolve().parents[1]
HOST_MSL = Path('/tmp/np-hdr-shader/host-0.metal')

# kernel id -> (name, expected defines)
KERNELS = {
    1: ('catmullrom', ('NP_SUPPORT_DOWN 2.0f', 'NP_B 0.0f', 'NP_C 0.5f')),
    2: ('mitchell', ('NP_SUPPORT_DOWN 2.0f', 'NP_B 0.33333334f', 'NP_C 0.33333334f')),
    3: ('keys075', ('NP_SUPPORT_DOWN 2.0f', 'NP_B 0.0f', 'NP_C 0.75f')),
    4: ('lanczos2', ('NP_SUPPORT_DOWN 2.0f', 'NP_A 2.0f')),
    5: ('lanczos3', ('NP_SUPPORT_DOWN 3.0f', 'NP_A 3.0f')),
    6: ('bspline', ('NP_SUPPORT_DOWN 2.0f', 'NP_B 1.0f', 'NP_C 0.0f')),
    7: ('spline36', ('NP_SUPPORT_DOWN 3.0f', '453.0f / 209.0f')),
    8: ('spline64', ('NP_SUPPORT_DOWN 4.0f', '6387.0f / 2911.0f')),
}


@pytest.mark.parametrize("probe", ["pp_scale_render_probe.m", "pp_draw_concurrency_probe.m"])
def test_scaling_on_metal(tmp_path, probe):
    """Read back the production two-pass output and each generated kernel."""
    generate_header(
        ROOT / 'manifests', tmp_path / 'renderer-pp-hdr-bindings.h', 'renderer-pp-hdr')
    output = tmp_path / 'render-probe'
    subprocess.run([
        'xcrun', 'clang', '-O1', '-I' + str(tmp_path),
        str(ROOT / 'tests' / probe),
        '-framework', 'Foundation', '-framework', 'QuartzCore',
        '-framework', 'CoreGraphics', '-framework', 'Metal', '-o', str(output),
    ], check=True)
    subprocess.run([str(output)], check=True)


def test_tap_pairing_is_exact():
    """The folded fetch must equal the four separate taps it replaces."""
    output = ROOT / 'build' / 'pp-scale-pairing-probe'
    subprocess.run([
        'xcrun', 'clang', '-O1', '-Wall', '-Wextra',
        str(ROOT / 'tests/pp_scale_pairing_probe.c'), '-lm', '-o', str(output),
    ], check=True)
    subprocess.run([str(output)], check=True)


def test_kernel_math():
    """The generated MSL mirrors these functions; keep them in step."""
    output = ROOT / 'build' / 'pp-scale-kernel-probe'
    subprocess.run([
        'xcrun', 'clang', '-O1', '-Wall', '-Wextra',
        str(ROOT / 'tests/pp_scale_kernel_probe.c'), '-lm', '-o', str(output),
    ], check=True)
    subprocess.run([str(output)], check=True)


@pytest.mark.skipif(not HOST_MSL.is_file(), reason='no captured host shader')
def test_kernels_bake_into_the_shader(tmp_path):
    """Every kernel must produce MSL the Metal compiler accepts."""
    generate_header(
        ROOT / 'manifests', tmp_path / 'renderer-pp-hdr-bindings.h', 'renderer-pp-hdr')
    driver = tmp_path / 'rewrite'
    subprocess.run([
        'xcrun', 'clang', '-O1', '-I' + str(tmp_path),
        str(ROOT / 'tests/pp_scale_rewrite_driver.c'),
        '-framework', 'QuartzCore', '-framework', 'CoreGraphics',
        '-framework', 'Foundation', '-o', str(driver),
    ], check=True)

    for kernel, (name, defines) in KERNELS.items():
        shader = tmp_path / f'{name}.metal'
        subprocess.run([
            str(driver), str(HOST_MSL), str(shader), '16', '1', str(kernel),
        ], check=True)
        source = shader.read_text()
        for define in defines:
            assert define in source, f'{name} is missing {define}'
        # The host sampler is replaced by the tap loop.
        assert '.sample(s,' not in source
        assert 'npa_scale1(' in source and 'npa_scale2(' in source
        # Every kernel is separable, so the generated MSL carries the
        # horizontal pre-pass entry point and one loop per axis.
        assert 'fragment float4 npaHorizontal(' in source
        # The second copy is renamed by macro, so the text keeps the original
        # identifier and the preprocessor separates the two.
        assert '#define npa_tap npa_h_tap' in source
        assert '#define NP_HORIZONTAL 1' in source
        # The loop is one-dimensional now: no 2D tap count.
        assert 'a.k.x' not in source and 'a.k.y' not in source
        # Both directions are baked, because the drawable can be resized while
        # the shader stays alive.
        assert 'npa_w_down(' in source and 'npa_w_up(' in source
        assert 'NP_SUPPORT_DOWN' in source and 'NP_SUPPORT_UP' in source
        # And the result must compile.
        subprocess.run([
            'xcrun', '-sdk', 'iphoneos', 'metal', '-c', str(shader),
            '-o', str(tmp_path / f'{name}.air'),
        ], check=True)


@pytest.mark.skipif(not HOST_MSL.is_file(), reason='no captured host shader')
def test_antialiasing_and_antiringing_bake(tmp_path):
    """Compile representative AA, anti-ringing and mixed-kernel settings."""
    generate_header(
        ROOT / 'manifests', tmp_path / 'renderer-pp-hdr-bindings.h', 'renderer-pp-hdr')
    driver = tmp_path / 'rewrite'
    subprocess.run([
        'xcrun', 'clang', '-O1', '-I' + str(tmp_path),
        str(ROOT / 'tests/pp_scale_rewrite_driver.c'),
        '-framework', 'QuartzCore', '-framework', 'CoreGraphics',
        '-framework', 'Foundation', '-o', str(driver),
    ], check=True)

    cases = [
        (5, 5, 0, 0, 0, 0),
        (5, 5, 1, 1, 1, 0),
        (5, 5, 1, 1, 2, 0),
        (5, 6, 1, 0, 2, 0),
        (6, 5, 1, 1, 1, 0),
    ]
    cases += [(0, 0, 0, 0, 2, 1), (5, 5, 0, 0, 2, 1), (8, 8, 0, 0, 2, 1)]
    for down, up, ar_down, ar_up, aa, deband in cases:
        name = f'aa{aa}-{down}-{up}-{ar_down}{ar_up}-deband{deband}'
        shader = tmp_path / f'{name}.metal'
        subprocess.run([
            str(driver), str(HOST_MSL), str(shader), '16', '1',
            str(down), str(up), str(ar_down), str(ar_up), str(aa), str(deband),
        ], check=True)
        subprocess.run([
            'xcrun', '-sdk', 'iphoneos', 'metal', '-c', str(shader),
            '-o', str(tmp_path / f'{name}.air'),
        ], check=True)


@pytest.mark.skipif(not HOST_MSL.is_file(), reason='no captured host shader')
def test_kernel_zero_keeps_bilinear(tmp_path):
    generate_header(
        ROOT / 'manifests', tmp_path / 'renderer-pp-hdr-bindings.h', 'renderer-pp-hdr')
    driver = tmp_path / 'rewrite'
    subprocess.run([
        'xcrun', 'clang', '-O1', '-I' + str(tmp_path),
        str(ROOT / 'tests/pp_scale_rewrite_driver.c'),
        '-framework', 'QuartzCore', '-framework', 'CoreGraphics',
        '-framework', 'Foundation', '-o', str(driver),
    ], check=True)
    shader = tmp_path / 'off.metal'
    subprocess.run([str(driver), str(HOST_MSL), str(shader), '16', '1', '0'], check=True)
    source = shader.read_text()
    assert 'npa_scale' not in source
    assert '.sample(s,' in source
