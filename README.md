# Frill

GLSL build system. Frill automatically resolves include dependencies for GLSL shader files and builds outdated targets
in parallel. All shader variants for different flag combinations will be built.

> WARNING: this project is under development

## Platforms

+ macOS
+ Linux
+ Windows

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

One should put a configuration file called `frill.json` in each source file directory and all relevant subdirectories.
Build targets/multi-compile flags/include directories are specified in `frill.json`. See `samples/frill.json` for more
details.

To load compiled SPIR-V code, link `frill_archive`, and call `frill::IArchive->load`. `frill_archive` does not link
with `shaderc`, your application size won't explode for that (`shaderc` is not that big under release build, though).

## Predefined Macro

Frill add default macros depending on the shader stage inferred from file extension.

| extension | macro                              |
|-----------|------------------------------------|
| .vert     | FRILL_SHADER_STAGE_VERT            |
| .frag     | FRILL_SHADER_STAGE_FRAG            |
| .tesc     | FRILL_SHADER_STAGE_TESS_CONTROL    |
| .tese     | FRILL_SHADER_STAGE_TESS_EVALUATION |
| .geom     | FRILL_SHADER_STAGE_GEOM            |
| .comp     | FRILL_SHADER_STAGE_COMP            |
| .rgen     | FRILL_SHADER_STAGE_RAY_GEN         |
| .rahit    | FRILL_SHADER_STAGE_ANY_HIT         |
| .rchit    | FRILL_SHADER_STAGE_CLOSEST_HIT     |
| .rmiss    | FRILL_SHADER_STAGE_MISS            |
| .rint     | FRILL_SHADER_STAGE_INTERSECTION    |
| .rcall    | FRILL_SHADER_STAGE_CALLABLE        |
| .task     | FRILL_SHADER_STAGE_TASK            |
| .mesh     | FRILL_SHADER_STAGE_MESH            |

For `.glsl` file, one can manually specify a shader stage macro listed above to hint the compiler the shader kind.