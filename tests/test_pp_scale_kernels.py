import subprocess
from pathlib import Path

import pytest
from npabridge.renderer_bindings import generate_header

ROOT = Path(__file__).resolve().parents[1]
HOST_MSL = Path('/tmp/np-hdr-shader/host-0.metal')

# kernel id -> (name, expected defines)
KERNELS = {
    1: ('catmullrom', ('NP_SUPPORT 2.0f', 'NP_B 0.0f', 'NP_C 0.5f')),
    2: ('mitchell', ('NP_SUPPORT 2.0f', 'NP_B 0.33333334f', 'NP_C 0.33333334f')),
    3: ('keys075', ('NP_SUPPORT 2.0f', 'NP_B 0.0f', 'NP_C 0.75f')),
    4: ('lanczos2', ('NP_SUPPORT 2.0f', 'NP_A 2.0f')),
    5: ('lanczos3', ('NP_SUPPORT 3.0f', 'NP_A 3.0f')),
}


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
        assert 'dfdx(texcoord)' in source and 'dfdy(texcoord)' in source
        # And the result must compile.
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
