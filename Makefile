UV ?= uv

.PHONY: bootstrap deps bridge verify test smoke clean

bootstrap:
	$(UV) sync --frozen --all-groups
	$(UV) run python tools/keystone.py

deps:
	$(UV) run python deps/build_deps.py
	$(UV) run python deps/build_ffmpeg.py
	$(UV) run python deps/build_ffmpeg_core.py
	$(UV) run python deps/build_ffmpeg_core.py --lock deps/ffmpeg-core902.lock.json

bridge:
	$(UV) run python -m npabridge.build_bridge

verify:
	$(UV) run python -m npabridge.build_bridge --verify-only

test:
	$(UV) run pytest

smoke:
	$(UV) run python dev/tools/smoke.py

clean:
	rm -rf build dist
