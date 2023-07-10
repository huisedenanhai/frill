# Frill

GLSL build system. Frill automatically resolves include dependencies for GLSL shader files and builds outdated targets
in parallel. All shader variants for different flag combinations will be built.

> WARNING: this project is under development

## Platforms

+ macOS
+ Linux
+ Windows

## Usage

```
Usage: frill-rs [OPTIONS] --output-dir <OUTPUT_DIR> --source-dir <SOURCE_DIR>

Options:
  -o, --output-dir <OUTPUT_DIR>
  -s, --source-dir <SOURCE_DIR>
  -c, --cache-dir <CACHE_DIR>
  -f, --force-rebuild
  -h, --help                     Print help
```

Run `frill -h` for more details.

One should put a configuration file called `frill.json` in each source file directory and all relevant subdirectories.
Build targets/multi-compile flags/include directories are specified in `frill.json`. See `samples/frill.json` for more
details.

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