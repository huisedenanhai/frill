use std::io::{Read, Write};

use anyhow::Ok;
use itertools::Itertools;

use crate::{Task, TaskOutput};

#[derive(Hash)]
pub struct PackCpp {}

impl Task for PackCpp {
    fn version(&self) -> u64 {
        0
    }

    fn summary(&self) -> String {
        format!("Packing assets to {},{}", self.h_name(), self.cpp_name())
    }

    fn run(&self, ctx: &mut crate::Context) -> anyhow::Result<Vec<crate::TaskOutput>> {
        let mut index_ctor: Vec<String> = vec![];
        let mut all_bytes: Vec<u8> = vec![];

        let index_file = ctx.open("index.json")?;
        let indices: Vec<TaskOutput> = serde_json::from_reader(index_file)?;

        for TaskOutput { uri, file } in indices {
            let offset = all_bytes.len();
            let size = ctx.open(file)?.read_to_end(&mut all_bytes)?;

            index_ctor.push(format!(
                "indices.emplace(\"{}\", DataRange{{{}, {}}});",
                uri, offset, size
            ));

            // align data
            let alignment = 16;
            let aligned_size = ((all_bytes.len() + alignment - 1) / alignment) * alignment;
            all_bytes.append(&mut [0u8].repeat(aligned_size - all_bytes.len()));
            assert_eq!(all_bytes.len(), aligned_size);
        }

        let mut h_file = ctx.create(self.h_name())?;
        let mut cpp_file = ctx.create(self.cpp_name())?;

        h_file.write_all(include_str!("frill.h").as_bytes())?;
        cpp_file.write_all(
            include_str!("frill.cpp")
                .replace("//@INDEX", &index_ctor.join("\n"))
                .replace("//@BYTES", &all_bytes.iter().join(","))
                .as_bytes(),
        )?;

        Ok(vec![
            TaskOutput::from_file_name(self.h_name()),
            TaskOutput::from_file_name(self.cpp_name()),
        ])
    }
}

impl PackCpp {
    fn cpp_name(&self) -> String {
        "frill.cpp".into()
    }

    fn h_name(&self) -> String {
        "frill.h".into()
    }
}
