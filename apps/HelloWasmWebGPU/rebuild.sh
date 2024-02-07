HALIDE_INSTALL_DIR="${HALIDE_INSTALL_DIR:-/home/install/halide}"

rm -r -f build \
    && cmake -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HALIDE_INSTALL_DIR" \
        -DHalide_TARGET=wasm-32-wasmrt-webgpu \
        -S . \
        -B build \
    && ninja -v -C build
