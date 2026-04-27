use std::path::PathBuf;

type DMemLimit = std::collections::HashMap<String, u64>;

#[derive(Debug)]
pub struct CGroup {
    path: PathBuf,
}

impl CGroup {
    pub fn root() -> CGroup {
        CGroup {
            path: PathBuf::from("/sys/fs/cgroup"),
        }
    }

    pub fn is_root(&self) -> bool {
        self.path == std::path::Path::new("/sys/fs/cgroup/")
    }

    pub fn from_path(path: PathBuf) -> CGroup {
        assert!(path.starts_with("/sys/fs/cgroup/"));

        CGroup { path }
    }

    pub fn descendants(&self) -> Vec<CGroup> {
        let mut descs: Vec<CGroup> = Vec::new();

        let dir = std::fs::read_dir(&self.path);
        if let Ok(dir) = dir {
            for entry in dir {
                if entry.is_err() {
                    continue;
                }
                let entry = entry.unwrap();
                let file_type = entry.file_type();

                if file_type.is_err() || !file_type.unwrap().is_dir() {
                    continue;
                }

                descs.push(CGroup::from_path(entry.path()));
            }
        }

        descs
    }

    pub fn name(&self) -> String {
        if self.is_root() {
            String::from("")
        } else if let Some(name) = self.path.file_name() {
            name.to_string_lossy().to_string()
        } else {
            String::from("")
        }
    }

    pub fn parent(&self) -> Option<CGroup> {
        if self.is_root() {
            return None;
        }

        let parent = self.path.parent();

        if let None = parent {
            return None;
        }

        Some(CGroup::from_path(parent?.to_path_buf()))
    }

    pub fn active_controllers(&self) -> Option<Vec<String>> {
        if let Ok(str) = std::fs::read_to_string(self.path.join("cgroup.subtree_control")) {
            let str = str.trim();
            Some(
                str.split(' ')
                    .filter_map(|x| {
                        if x.is_empty() {
                            None
                        } else {
                            Some(x.to_string())
                        }
                    })
                    .collect(),
            )
        } else {
            None
        }
    }

    pub fn add_controller(&mut self, controller: &str) -> Result<(), std::io::Error> {
        let mut control = String::from("+");
        control.push_str(controller);
        std::fs::write(self.path.join("cgroup.subtree_control"), control)
    }

    fn parse_limits_file<P: AsRef<std::path::Path>>(file: P) -> Option<DMemLimit> {
        if let Ok(str) = std::fs::read_to_string(file) {
            let mut limit = DMemLimit::new();
            for line in str.lines() {
                let words: Vec<_> = line.split(' ').collect();
                if words.len() != 2 {
                    println!(
                        "WARNING: Unexpected number of words in dmem limit string: \"{}\"\n",
                        line
                    );
                    continue;
                }
                if words[1] == "max" {
                    limit.insert(words[0].to_string(), u64::max_value());
                } else if let Ok(val) = u64::from_str_radix(words[1], 10) {
                    limit.insert(words[0].to_string(), val);
                } else {
                    println!("WARNING: Could not parse dmem limit number: \"{}\"\n", line);
                }
            }
            Some(limit)
        } else {
            None
        }
    }

    fn write_limits_file<P: AsRef<std::path::Path>>(&self, file: P, limit: &DMemLimit) {
        let mut contents = String::new();

        for entry in limit {
            contents.push_str(entry.0.as_str());
            contents.push_str(" ");
            if *entry.1 == u64::max_value() {
                contents.push_str("max");
            } else {
                contents.push_str(entry.1.to_string().as_str());
            }
            contents.push('\n');
        }

        if let Err(e) = std::fs::write(file, contents) {
            if e.kind() != std::io::ErrorKind::PermissionDenied {
                println!("WARNING: Could not write dmem limit file: {}!", e);
            }
        }
    }

    fn limit_from_attribute(&self, attrib_name: &str) -> Option<DMemLimit> {
        let mut file: PathBuf = self.path.clone();
        file.push(attrib_name);
        Self::parse_limits_file(file)
    }

    pub fn device_memory_capacity(&self) -> Option<DMemLimit> {
        self.limit_from_attribute("dmem.capacity")
    }

    pub fn write_device_memory_low(&mut self, limit: &DMemLimit) {
        let mut file: PathBuf = self.path.clone();
        file.push("dmem.low");
        self.write_limits_file(file, limit);
    }
}
