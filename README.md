# Frill

GLSL build system. Frill automatically resolves include dependencies for GLSL shader files and builds outdated targets
in parallel. All shader variants for different flag combinations will be built.

It should work on macOS and Windows, as I use Frill on these platforms.

> WARNING: this project is under development

## Download

```bash
git clone --recursive https://github.com/huisedenanhai/frill.git 
cd frill/third_party/shaderc
python utils/git-sync-deps
```

Update submodules for already cloned repos:

```bash
cd frill
git submodule update --init --recursive
```

## Build

Just build it with default cmake options.

## Usage

```
frill [--cache-dir=STRING] [--output-dir=STRING] [--source-dir=STRING] [--thread-count=INTEGER]
```

Run `frill -h` for more details.

One should put a configuration file called `frill.json` in each source file directory and all relevant subdirectories. Build targets/multi-compile flags/include directories are specified in `frill.json`. See `samples/frill.json` for more details.

To load compiled SPIR-V code, link `frill_archive`, and call `frill::IArchive->load`. `frill_archive` does not link with `shaderc`, your application size won't explode for that (`shaderc` is not that big under release build, though).
