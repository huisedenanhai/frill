mod tasks;
use clap::Parser;
use filetime::FileTime;
use near_stable_hasher::StableHasher;
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::{
    cell::RefCell,
    fs::{File, Metadata},
    hash::{Hash, Hasher},
    io::Read,
    mem::ManuallyDrop,
    ops::Deref,
    path::{Path, PathBuf},
    rc::Rc,
};

use crate::tasks::compile_glsl::load_glsl_tasks;

#[derive(Debug, Serialize, Deserialize, Default, PartialEq, Eq)]
struct Timestamp {
    unix_seconds: i64,
    nano_seconds: u32,
}

impl Timestamp {
    fn from_metadata(metadata: &Metadata) -> Timestamp {
        let time = FileTime::from_last_modification_time(metadata);
        Timestamp {
            unix_seconds: time.unix_seconds(),
            nano_seconds: time.nanoseconds(),
        }
    }
}

#[derive(Debug, Serialize, Deserialize, Default)]
struct Dependency {
    path: PathBuf,
    time_stamp: Timestamp,
}

#[derive(Debug, Serialize, Deserialize, Default)]
struct Cache {
    deps: Vec<Dependency>,
}

impl Cache {
    fn add_deps(&mut self, path: &Path) -> anyhow::Result<()> {
        self.deps.push(Dependency {
            path: path.canonicalize()?,
            time_stamp: Timestamp::from_metadata(&std::fs::metadata(path)?),
        });
        Ok(())
    }
}

// Record file dependency after file is closed, otherwise the write time stamp might be incorrect
pub struct FileHandle {
    file: ManuallyDrop<File>,
    path: PathBuf,
    cache: Rc<RefCell<Cache>>,
}

impl Drop for FileHandle {
    fn drop(&mut self) {
        // close the file
        unsafe {
            std::mem::ManuallyDrop::drop(&mut self.file);
        }
        self.cache.borrow_mut().add_deps(&self.path).unwrap();
    }
}

impl std::io::Read for FileHandle {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        self.file.read(buf)
    }
}

impl std::io::Write for FileHandle {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.file.write(buf)
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.file.flush()
    }
}

impl Deref for FileHandle {
    type Target = File;

    fn deref(&self) -> &Self::Target {
        &self.file
    }
}

#[derive(Debug)]
pub struct Context {
    cache: Rc<RefCell<Cache>>,
    cwd: Vec<PathBuf>,
}

impl Context {
    pub fn new(cwd: PathBuf) -> Context {
        Context {
            cache: Default::default(),
            cwd: vec![cwd],
        }
    }

    fn cwd(&self) -> &Path {
        self.cwd.last().unwrap()
    }

    fn resolve_path(&self, path: &Path) -> PathBuf {
        if path.is_absolute() {
            path.into()
        } else {
            self.cwd().join(path)
        }
    }

    pub fn open<P: AsRef<Path>>(&mut self, path: P) -> anyhow::Result<FileHandle> {
        let path = self.resolve_path(path.as_ref());
        let file = File::open(&path)
            .map_err(|err| anyhow::anyhow!("failed to open {}: {}", path.display(), err))?;
        Ok(FileHandle {
            file: ManuallyDrop::new(file),
            path,
            cache: self.cache.clone(),
        })
    }

    pub fn create<P: AsRef<Path>>(&mut self, path: P) -> anyhow::Result<FileHandle> {
        let path = self.resolve_path(path.as_ref());
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?
        }
        let file = File::create(&path)
            .map_err(|err| anyhow::anyhow!("failed to create {}: {}", path.display(), err))?;
        Ok(FileHandle {
            file: ManuallyDrop::new(file),
            path,
            cache: self.cache.clone(),
        })
    }

    pub fn read_to_string<P: AsRef<Path>>(&mut self, path: P) -> anyhow::Result<String> {
        let mut content = String::default();
        let path = path.as_ref();
        self.open(path)?
            .read_to_string(&mut content)
            .map_err(|err| anyhow::anyhow!("failed to read from {}: {}", path.display(), err))?;
        Ok(content)
    }

    pub fn push_cwd(&mut self, cwd: PathBuf) {
        self.cwd.push(cwd);
    }

    pub fn pop_cwd(&mut self) {
        self.cwd.pop();
    }
}

pub trait Task: Hash {
    fn summary(&self) -> String;
    fn run(&self, ctx: &mut Context) -> anyhow::Result<()>;
}

#[derive(Debug, Parser)]
struct Options {
    #[arg(short, long)]
    output_dir: PathBuf,
    #[arg(short, long)]
    source_dir: PathBuf,
    #[arg(short, long)]
    cache_dir: Option<PathBuf>,
    #[arg(short, long, default_value_t = false)]
    force_rebuild: bool,
}

impl Options {
    fn check_outdated_impl(&self, task: &impl Task) -> anyhow::Result<bool> {
        let cache_file_content =
            std::fs::read_to_string(self.get_cache_dir().join(task.cache_file_name()))?;
        let cache: Cache = serde_json::from_str(&cache_file_content)?;

        for dep in cache.deps {
            let metadata = std::fs::metadata(&dep.path)?;
            let time = Timestamp::from_metadata(&metadata);
            if time != dep.time_stamp {
                return Ok(true);
            }
        }

        Ok(false)
    }

    fn check_outdated(&self, task: &impl Task) -> bool {
        self.check_outdated_impl(task).unwrap_or(true)
    }

    fn get_cache_dir(&self) -> PathBuf {
        let default_cache_dir = self.output_dir.join("__frill_cache__");
        self.cache_dir
            .as_ref()
            .unwrap_or(&default_cache_dir)
            .clone()
    }

    fn execute_task_impl(
        &self,
        index: usize,
        count: usize,
        task: &impl Task,
    ) -> anyhow::Result<()> {
        if !self.force_rebuild && !self.check_outdated(task) {
            return Ok(());
        }
        let summary = task.summary();
        let digit_count = count.checked_ilog10().unwrap_or(0) as usize + 1;

        println!(
            "[{:digit_count$}/{:digit_count$}] {}",
            index + 1,
            count,
            summary,
            digit_count = digit_count
        );

        let mut ctx = Context::new(self.output_dir.clone());
        let result = task.run(&mut ctx).map_err(|info| {
            eprintln!("Error when {}: {}", task.summary(), info);
            info
        });

        // We can not get correct modification time stamp of the cache file before actually write to it.
        // For now we do not track timestamp of the cache file :(
        let cache_json = serde_json::to_string_pretty(&ctx.cache.borrow() as &Cache)?;
        std::fs::create_dir_all(self.get_cache_dir())?;
        std::fs::write(
            self.get_cache_dir().join(task.cache_file_name()),
            cache_json,
        )?;

        result
    }

    fn execute_task(&self, index: usize, count: usize, task: &impl Task) -> bool {
        self.execute_task_impl(index, count, task)
            .map_err(|info| {
                eprintln!("Error when {}: {}", task.summary(), info);
                info
            })
            .is_ok()
    }

    fn execute_tasks<T>(&self, tasks: &[T]) -> bool
    where
        T: Task + Sync + Send,
    {
        let count = tasks.len();
        tasks
            .par_iter()
            .enumerate()
            .map(|(index, task)| self.execute_task(index, count, task))
            .all(|x| x)
    }
}

pub trait HashExt {
    fn calc_hash(&self) -> u64;
    fn cache_file_name(&self) -> String;
}

impl<T: Hash> HashExt for T {
    fn calc_hash(&self) -> u64 {
        let mut hasher = StableHasher::default();
        self.hash(&mut hasher);
        hasher.finish()
    }

    fn cache_file_name(&self) -> String {
        format!("{:X}.json", self.calc_hash())
    }
}

fn main() {
    let options = Options::parse();
    let glsl_tasks = load_glsl_tasks(&options.source_dir, &[]).unwrap();
    options.execute_tasks(&glsl_tasks);
    println!("Hello, world!");
}
