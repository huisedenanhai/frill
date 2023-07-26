use std::{
    cell::RefCell,
    collections::BTreeSet,
    hash::Hash,
    io::Write,
    path::{Path, PathBuf},
};

use itertools::Itertools;
use serde::{Deserialize, Serialize};
use shaderc::{IncludeCallbackResult, ShaderKind};

use crate::{HashExt, Task, TaskOutput};

#[derive(Hash, Debug, Default)]
pub struct GLSLCompileTask {
    file: PathBuf,
    include_dirs: BTreeSet<PathBuf>,
    defines: BTreeSet<String>,
}

impl Task for GLSLCompileTask {
    fn summary(&self) -> String {
        format!(
            "Compiling {} {}",
            self.file.to_string_lossy(),
            self.defines.iter().join(",")
        )
    }

    fn run(&self, ctx: &mut crate::Context) -> anyhow::Result<Vec<TaskOutput>> {
        let ctx = RefCell::new(ctx);
        let mut options = shaderc::CompileOptions::new().unwrap();

        if let Some(stage_macro) = self.stage_macro() {
            options.add_macro_definition(&stage_macro, None);
        }

        for def in &self.defines {
            options.add_macro_definition(def, None);
        }

        options.set_include_callback(
            |requested_src, include_type, requesting_src, include_depth| -> IncludeCallbackResult {
                let max_include_depth = 50;
                if include_depth > max_include_depth {
                    return Err(format!("include depth exceeds {}", max_include_depth));
                }
                let requesting_src: PathBuf = requesting_src.into();
                let mut include_dirs = vec![];
                if matches!(include_type, shaderc::IncludeType::Relative) {
                    if let Some(parent) = requesting_src.parent() {
                        include_dirs.push(parent);
                    }
                }

                for include_dir in &self.include_dirs {
                    include_dirs.push(include_dir);
                }

                let result = include_dirs
                    .iter()
                    .map(|folder| folder.join(requested_src))
                    .filter_map(|path| {
                        if path.exists() {
                            let path = path.canonicalize().unwrap();
                            ctx.borrow_mut()
                                .read_to_string(&path)
                                .ok()
                                .map(|content| (path, content))
                        } else {
                            None
                        }
                    })
                    .next();

                if let Some((resolved_name, content)) = result {
                    Ok(shaderc::ResolvedInclude {
                        resolved_name: resolved_name.to_string_lossy().to_string(),
                        content,
                    })
                } else {
                    Err("failed to resolve include".into())
                }
            },
        );

        let compiler = shaderc::Compiler::new().unwrap();

        let source_text = ctx.borrow_mut().read_to_string(&self.file)?;
        let artifact = compiler.compile_into_spirv(
            &source_text,
            self.shader_kind()?,
            &self.file.to_string_lossy(),
            "main",
            Some(&options),
        )?;

        let mut out_file = ctx.borrow_mut().create(self.output_file_name())?;
        out_file.write_all(artifact.as_binary_u8())?;
        let uri = self.uri(ctx.borrow().source_root_dir());
        Ok(vec![TaskOutput {
            uri,
            file: self.output_file_name().into(),
        }])
    }

    fn version(&self) -> u64 {
        0
    }
}

const SHADER_EXTENSIONS: &[(&str, &str, ShaderKind)] = &[
    ("vert", "FRILL_SHADER_STAGE_VERT", ShaderKind::Vertex),
    ("frag", "FRILL_SHADER_STAGE_FRAG", ShaderKind::Fragment),
    (
        "tesc",
        "FRILL_SHADER_STAGE_TESS_CONTROL",
        ShaderKind::TessControl,
    ),
    (
        "tese",
        "FRILL_SHADER_STAGE_TESS_EVALUATION",
        ShaderKind::TessEvaluation,
    ),
    ("geom", "FRILL_SHADER_STAGE_GEOM", ShaderKind::Geometry),
    ("comp", "FRILL_SHADER_STAGE_COMP", ShaderKind::Compute),
    (
        "rgen",
        "FRILL_SHADER_STAGE_RAY_GEN",
        ShaderKind::RayGeneration,
    ),
    ("rahit", "FRILL_SHADER_STAGE_ANY_HIT", ShaderKind::AnyHit),
    (
        "rchit",
        "FRILL_SHADER_STAGE_CLOSEST_HIT",
        ShaderKind::ClosestHit,
    ),
    ("rmiss", "FRILL_SHADER_STAGE_MISS", ShaderKind::Miss),
    (
        "rint",
        "FRILL_SHADER_STAGE_INTERSECTION",
        ShaderKind::Intersection,
    ),
    ("rcall", "FRILL_SHADER_STAGE_CALLABLE", ShaderKind::Callable),
    ("task", "FRILL_SHADER_STAGE_TASK", ShaderKind::Task),
    ("mesh", "FRILL_SHADER_STAGE_MESH", ShaderKind::Mesh),
];

impl GLSLCompileTask {
    fn new(file: PathBuf, include_dirs: Vec<PathBuf>, defines: Vec<String>) -> Self {
        Self {
            file,
            include_dirs: include_dirs.into_iter().collect(),
            defines: defines.into_iter().collect(),
        }
    }

    fn uri(&self, root_folder: &Path) -> String {
        format!(
            "{}/flags={}",
            self.file
                .strip_prefix(root_folder)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/"),
            self.defines.iter().join(",")
        )
    }

    fn output_file_name(&self) -> String {
        let hash = self.calc_hash();
        format!("{:X}.spv", hash)
    }

    fn stage_macro(&self) -> Option<String> {
        if let Some(Some(ext)) = self.file.extension().map(|os_str| os_str.to_str()) {
            if let Some(flag) = SHADER_EXTENSIONS
                .iter()
                .find_map(|(stage_ext, flag, _)| (*stage_ext == ext).then(|| flag.to_string()))
            {
                return Some(flag);
            }
        }

        None
    }

    fn shader_kind(&self) -> anyhow::Result<shaderc::ShaderKind> {
        if let Some(Some(ext)) = self.file.extension().map(|os_str| os_str.to_str()) {
            if let Some(kind) = SHADER_EXTENSIONS
                .iter()
                .find_map(|(stage_ext, _, kind)| (*stage_ext == ext).then_some(kind))
            {
                return Ok(*kind);
            }

            if ext == "glsl" {
                // Use flags to determine shader stage
                if let Some(kind) = self.defines.iter().find_map(|flag| {
                    SHADER_EXTENSIONS
                        .iter()
                        .find_map(|(_, stage_flag, kind)| (*stage_flag == flag).then_some(kind))
                }) {
                    return Ok(*kind);
                } else {
                    return Err(anyhow::anyhow!(
                        "Should specify a stage macro for *.glsl file"
                    ));
                }
            }
        }

        Err(anyhow::anyhow!("invalid shader file extension"))
    }
}

#[derive(Serialize, Deserialize, Clone)]
struct MultiCompileEntryDetailed {
    options: Vec<String>,
    can_off: bool,
}

impl From<MultiCompileEntry> for MultiCompileEntryDetailed {
    fn from(value: MultiCompileEntry) -> Self {
        match value {
            MultiCompileEntry::Flag(flag) => MultiCompileEntryDetailed {
                options: vec![flag],
                can_off: true,
            },
            MultiCompileEntry::MultiFlags(options) => MultiCompileEntryDetailed {
                options,
                can_off: true,
            },
            MultiCompileEntry::Detailed(value) => value,
        }
    }
}

#[derive(Serialize, Deserialize, Clone)]
#[serde(untagged)]
enum MultiCompileEntry {
    Flag(String),
    MultiFlags(Vec<String>),
    Detailed(MultiCompileEntryDetailed),
}

#[derive(Serialize, Deserialize)]
struct SourceInfoDetailed {
    file: PathBuf,
    multi_compile: Option<Vec<MultiCompileEntry>>,
    includes: Option<Vec<PathBuf>>,
}

impl SourceInfoDetailed {
    fn get_tasks(&self, path: &Path, includes: &[PathBuf]) -> anyhow::Result<Vec<GLSLCompileTask>> {
        let mut result = vec![];
        let mut includes = includes.to_vec();
        if let Some(additional_includes) = &self.includes {
            for additional_include in additional_includes {
                includes.push(resolve_path(path, additional_include)?);
            }
        }
        let file = resolve_path(path, &self.file)?;
        self.emit_multi_compile_tasks(&file, &includes, &mut vec![], 0, &mut result);
        Ok(result)
    }

    fn emit_multi_compile_tasks(
        &self,
        file: &Path,
        includes: &[PathBuf],
        flag_stack: &mut Vec<String>,
        index: usize,
        result: &mut Vec<GLSLCompileTask>,
    ) {
        let multi_compiles = vec![];
        let multi_compiles = self.multi_compile.as_ref().unwrap_or(&multi_compiles);

        if index >= multi_compiles.len() {
            result.push(GLSLCompileTask::new(
                file.into(),
                includes.into(),
                flag_stack.clone(),
            ));

            return;
        }

        let multi_compile: MultiCompileEntryDetailed = multi_compiles[index].clone().into();

        for flag in multi_compile.options {
            flag_stack.push(flag);
            self.emit_multi_compile_tasks(file, includes, flag_stack, index + 1, result);
            flag_stack.pop();
        }

        if multi_compile.can_off {
            self.emit_multi_compile_tasks(file, includes, flag_stack, index + 1, result);
        }
    }
}

impl From<SourceInfo> for SourceInfoDetailed {
    fn from(value: SourceInfo) -> Self {
        match value {
            SourceInfo::Name(name) => SourceInfoDetailed {
                file: name,
                multi_compile: None,
                includes: None,
            },
            SourceInfo::Detailed(value) => value,
        }
    }
}

#[derive(Serialize, Deserialize)]
#[serde(untagged)]
enum SourceInfo {
    Name(PathBuf),
    Detailed(SourceInfoDetailed),
}

#[derive(Serialize, Deserialize)]
struct FrillConfig {
    sources: Option<Vec<SourceInfo>>,
    includes: Option<Vec<PathBuf>>,
    subdirectories: Option<Vec<PathBuf>>,
}

fn resolve_path(base: &Path, path: &Path) -> anyhow::Result<PathBuf> {
    if path.is_absolute() {
        Ok(path.canonicalize()?)
    } else {
        Ok(base.join(path).canonicalize()?)
    }
}

pub fn load_glsl_tasks(path: &Path, includes: &[PathBuf]) -> anyhow::Result<Vec<GLSLCompileTask>> {
    let config_file = path.join("frill.json");
    let format_err =
        |err: String| anyhow::anyhow!("failed to parse {}: {}", config_file.display(), err);
    let config =
        std::fs::read_to_string(&config_file).map_err(|err| format_err(err.to_string()))?;
    let config: FrillConfig =
        serde_json::from_str(&config).map_err(|err| format_err(format!("{}", err)))?;

    let mut includes = includes.to_vec();
    if let Some(config_includes) = config.includes {
        for include_dir in config_includes {
            includes.push(resolve_path(path, &include_dir)?);
        }
    }

    let mut tasks = vec![];

    if let Some(sources) = config.sources {
        for source in sources {
            let detailed: SourceInfoDetailed = source.into();
            tasks.append(&mut detailed.get_tasks(path, &includes)?);
        }
    }

    if let Some(config_subdirectories) = config.subdirectories {
        for subdir in config_subdirectories {
            let subdir = resolve_path(path, &subdir)?;
            tasks.append(&mut load_glsl_tasks(&subdir, &includes)?);
        }
    }

    Ok(tasks)
}
