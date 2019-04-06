# CocoaCodec2

Cocoa Framework for the [Codec 2](http://rowetel.com/codec2.html) low bit-rate speech codec. Provides both macOS and iOS targets and is [Carthage](https://github.com/Carthage/Carthage) compatible.

Used by [CocoaDV](https://github.com/chazapis/CocoaDV) and, by extension, Estrella, the ORF reflector client for [macOS](https://github.com/chazapis/Estrella-macOS) and [iOS](https://github.com/chazapis/Estrella-iOS).

## Production notes

The included files were copied over from `r3898` (2018-11-02) of the Codec 2 [SVN repository](https://svn.code.sf.net/p/freetel/code/codec2/branches/).

I first compiled Codec 2 on macOS. I used [MacPorts](https://www.macports.org) to install `cmake`, `speexDSP`, and `libsamplerate`. To build, I had to `export LIBRARY_PATH=$LIBRARY_PATH:/opt/local/lib` before running `make`, and edit the following files to remove unsupported `gcc` flags (from the `build` folder):
* `unittest/CMakeFiles/ofdm_stack.dir/flags.make`, to remove `-fstack-usage`
* `unittest/CMakeFiles/ofdm_stack.dir/link.txt`, to remove `-Wl,-Map=ofdm_stack.map`

Then, I copied over:
* All files from `src`
* `codebook*.c` from `build`
* `version.h` from `build/codec2`

In Xcode:
* In the targets’ *Build Phases* tab, under *Headers*, under *Public*, add all the files listed in `src/CMakeLists.txt` under `CODEC2_PUBLIC_HEADERS`
* In the project's *Build Settings* tab, under *Apple Clang - Preprocessing*, under *Preprocessor Macros*, add `HORUS_L2_RX=1`, `INTERLEAVER=1`, `SCRAMBLER=1`, and `RUN_TIME_TABLES=1`
* Fix the include in `codec2.h`
* Remove all files producing executables (the ones used in `add_executable()` calls in `src/CMakeLists.txt`)
* Clean up, by keeping all files listed in `src/CMakeLists.txt` under `CODEC2_SRCS` and all files referenced by them

---

73 de SV9OAN
