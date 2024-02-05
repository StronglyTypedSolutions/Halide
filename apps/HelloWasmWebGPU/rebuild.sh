rm build -r \
    && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/home/install/halide" -S . -B build \
    && ninja -v -C build
