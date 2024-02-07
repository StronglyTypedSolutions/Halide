This app is a demo of using the Halide webassembly backend with WebGPU

To try it out,

- install emscripten (see emscripten.org), and have emcc in your path.

- set HALIDE_INSTALL_DIR to your Halide installation directory.
  
- Build with `./rebuild-sh`

- Run a local webserver using, e.g.: `python3 -m http.server 8080 --directory build`

- Load Google chrome (at least version 84), go to `chrome://flags`, and turn on all the experimental webassembly stuff (e.g. threads, simd). If you don't do this, only the single-threaded scalar variant will work (at the time of writing).

- Visit http://127.0.0.1:8080/index.html
