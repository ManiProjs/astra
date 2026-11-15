use gpui::{
    App, Application, Bounds, ClipboardItem, Context, FocusHandle, InteractiveElement, KeyBinding,
    Menu, MenuItem, SystemMenuType, Timer, Window, WindowBounds, WindowControlArea, WindowOptions,
    actions, div, img, point, prelude::*, px, rgb, size,
};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
    time::Duration,
};

const BG: u32 = 0x171717;
const SURFACE: u32 = 0x202020;
const PANEL: u32 = 0x242424;
const RAISED: u32 = 0x2b2b2b;
const BORDER: u32 = 0x353535;
const TEXT: u32 = 0xd8d8d8;
const MUTED: u32 = 0x858585;
const DIM: u32 = 0x626262;
const SELECTED: u32 = 0x3a3a3a;

actions!(
    astra_studio,
    [
        OpenProject,
        SaveDocument,
        RefreshProject,
        BuildProject,
        RunProject,
        GoHome,
        ShowLearn,
        ShowAbout,
        QuitStudio,
    ]
);

#[derive(Clone, Serialize, Deserialize)]
struct ProjectConfig {
    name: String,
    version: u32,
}

#[derive(Clone, Deserialize)]
struct ExampleInfo {
    target: String,
    title: String,
    source: String,
    description: String,
    platform: String,
    #[serde(default)]
    guide: Option<String>,
}

#[derive(Deserialize)]
struct LearningManifest {
    format: String,
    version: u32,
    examples: Vec<ExampleInfo>,
}

struct LearningComponent {
    root: PathBuf,
    examples: Vec<ExampleInfo>,
}

impl Default for ProjectConfig {
    fn default() -> Self {
        Self {
            name: "Astra Engine".into(),
            version: 1,
        }
    }
}

struct Studio {
    page: String,
    workspace: String,
    root: PathBuf,
    project: ProjectConfig,
    files: Vec<PathBuf>,
    selected: Option<PathBuf>,
    document: Option<PathBuf>,
    source: String,
    cursor: usize,
    selection: Option<std::ops::Range<usize>>,
    selection_anchor: Option<usize>,
    dirty: bool,
    editor_focus: FocusHandle,
    output: Vec<String>,
    status: String,
    active_example: String,
    confirm_unsaved: bool,
    pending_navigation: Option<PendingNavigation>,
    learning: Option<LearningComponent>,
}

#[derive(Clone)]
enum PendingNavigation {
    Home,
    Exit,
    OpenProject(PathBuf),
    OpenDocument(PathBuf),
    OpenExample(PathBuf, String),
}

impl Studio {
    fn new(cx: &mut Context<Self>) -> Self {
        let root = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
        let learning = discover_learning_component(&root);
        let project = fs::read_to_string(root.join("astra.project.json"))
            .ok()
            .and_then(|json| serde_json::from_str(&json).ok())
            .unwrap_or_default();
        let files = scan_project(&root);
        let starter = learning
            .as_ref()
            .and_then(|component| {
                component
                    .examples
                    .iter()
                    .find(|example| example.target == "astra_open_world")
                    .map(|example| component.root.join(&example.source))
            })
            .unwrap_or_else(|| root.join("examples/open_world/main.cpp"));
        let source = fs::read_to_string(&starter).unwrap_or_default();
        let this = Self {
            page: "home".into(),
            workspace: "3d".into(),
            root,
            project,
            files,
            selected: None,
            document: starter.is_file().then_some(starter),
            cursor: source.len(),
            selection: None,
            selection_anchor: None,
            source,
            dirty: false,
            editor_focus: cx.focus_handle(),
            output: vec![
                "Astra Studio ready".into(),
                "Project indexed · Studio ready".into(),
            ],
            status: "Ready".into(),
            active_example: "astra_open_world".into(),
            confirm_unsaved: false,
            pending_navigation: None,
            learning,
        };
        this
    }

    fn request_navigation(&mut self, destination: PendingNavigation) -> bool {
        if self.dirty {
            self.pending_navigation = Some(destination);
            self.confirm_unsaved = true;
            false
        } else {
            true
        }
    }

    fn finish_pending_navigation(&mut self, save: bool, window: &mut Window) {
        let Some(destination) = self.pending_navigation.clone() else {
            self.confirm_unsaved = false;
            return;
        };
        if save {
            self.save_document();
            if self.dirty {
                return;
            }
        } else {
            self.discard_document_changes();
        }
        self.confirm_unsaved = false;
        self.pending_navigation = None;
        match destination {
            PendingNavigation::Home => self.page = "home".into(),
            PendingNavigation::Exit => window.remove_window(),
            PendingNavigation::OpenProject(path) => self.open_project(path),
            PendingNavigation::OpenDocument(path) => {
                self.selected = Some(path);
                self.open_selected();
            }
            PendingNavigation::OpenExample(path, target) => {
                self.open_example_source_path(path, &target);
            }
        }
    }

    fn discard_document_changes(&mut self) {
        self.source = self
            .document
            .as_ref()
            .and_then(|path| fs::read_to_string(path).ok())
            .unwrap_or_default();
        self.cursor = self.source.len();
        self.selection = None;
        self.selection_anchor = None;
        self.dirty = false;
        self.status = "Unsaved changes discarded".into();
        self.output.push(self.status.clone());
    }

    fn open_project(&mut self, root: PathBuf) {
        self.root = root;
        self.project = fs::read_to_string(self.root.join("astra.project.json"))
            .ok()
            .and_then(|json| serde_json::from_str(&json).ok())
            .unwrap_or_else(|| ProjectConfig {
                name: self
                    .root
                    .file_name()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .into_owned(),
                version: 1,
            });
        self.files = scan_project(&self.root);
        self.selected = None;
        self.document = None;
        self.source.clear();
        self.cursor = 0;
        self.selection = None;
        self.selection_anchor = None;
        self.dirty = false;
        self.output
            .push(format!("Opened project {}", self.root.display()));
        self.status = format!("Project loaded · {} files indexed", self.files.len());
        self.page = "editor".into();
    }

    fn choose_project(&mut self, cx: &mut Context<Self>) {
        let receiver = cx.prompt_for_paths(gpui::PathPromptOptions {
            files: false,
            directories: true,
            multiple: false,
            prompt: Some("Open Astra project folder".into()),
        });
        cx.spawn(async move |this, cx| {
            if let Ok(Ok(Some(paths))) = receiver.await {
                if let Some(path) = paths.into_iter().next() {
                    let _ = this.update(cx, |this, cx| {
                        if this.dirty {
                            this.pending_navigation = Some(PendingNavigation::OpenProject(path));
                            this.confirm_unsaved = true;
                        } else {
                            this.open_project(path);
                        }
                        cx.notify();
                    });
                }
            }
        })
        .detach();
    }

    fn open_example_source(&mut self, source: &str, target: &str) {
        let path = self
            .learning
            .as_ref()
            .map(|component| component.root.join(source))
            .unwrap_or_else(|| self.root.join(source));
        if self.dirty {
            self.pending_navigation = Some(PendingNavigation::OpenExample(path, target.into()));
            self.confirm_unsaved = true;
            return;
        }
        self.open_example_source_path(path, target);
    }

    fn open_example_source_path(&mut self, path: PathBuf, target: &str) {
        match fs::read_to_string(&path) {
            Ok(text) => {
                self.document = Some(path.clone());
                self.selected = Some(path);
                self.source = text;
                self.cursor = 0;
                self.selection = None;
                self.selection_anchor = None;
                self.dirty = false;
                self.active_example = target.into();
                self.page = "editor".into();
                self.status = format!("Opened example source · {target}");
                self.output.push(self.status.clone());
            }
            Err(error) => self.status = format!("Could not open example source: {error}"),
        }
    }

    fn example_build_root(&self) -> Option<PathBuf> {
        let mut candidates = Vec::new();
        if let Some(path) = std::env::var_os("ASTRA_ENGINE_SOURCE_DIR") {
            candidates.push(PathBuf::from(path));
        }
        if let Some(component) = &self.learning {
            candidates.push(component.root.clone());
        }
        candidates.push(self.root.clone());
        candidates
            .into_iter()
            .find(|root| root.join("CMakeLists.txt").is_file() && root.join("examples").is_dir())
    }

    fn open_selected(&mut self) {
        let Some(path) = self.selected.clone() else {
            self.status = "Select a project file first".into();
            return;
        };
        if is_editable_source(&path) && self.dirty {
            self.pending_navigation = Some(PendingNavigation::OpenDocument(path));
            self.confirm_unsaved = true;
            return;
        }
        if is_editable_source(&path) {
            match fs::read_to_string(&path) {
                Ok(text) => {
                    self.document = Some(path.clone());
                    self.source = text;
                    self.cursor = self.source.len();
                    self.selection = None;
                    self.selection_anchor = None;
                    self.dirty = false;
                    self.status = format!("Opened {}", path.display());
                }
                Err(error) => self.status = format!("Read failed: {error}"),
            }
        } else {
            match Command::new("open").arg(&path).spawn() {
                Ok(_) => {
                    self.status =
                        format!("Opened {} with its associated application", path.display())
                }
                Err(error) => self.status = format!("Open failed: {error}"),
            }
        }
        self.output.push(self.status.clone());
    }

    fn save_document(&mut self) {
        let Some(path) = self.document.clone() else {
            self.status = "No document is open".into();
            return;
        };
        match fs::write(&path, &self.source) {
            Ok(()) => {
                self.dirty = false;
                self.status = format!("Saved {}", path.display());
            }
            Err(error) => self.status = format!("Save failed: {error}"),
        }
        self.output.push(self.status.clone());
    }

    fn edit_key(&mut self, key: &gpui::KeyDownEvent, cx: &mut Context<Self>) {
        let stroke = &key.keystroke;
        if stroke.modifiers.secondary() && stroke.key.eq_ignore_ascii_case("s") {
            self.save_document();
            return;
        }
        if stroke.modifiers.secondary() {
            match stroke.key.to_ascii_lowercase().as_str() {
                "a" => {
                    self.selection = Some(0..self.source.len());
                    self.selection_anchor = Some(0);
                    self.cursor = self.source.len();
                    return;
                }
                "c" | "x" => {
                    if let Some(range) = self.selection.clone() {
                        cx.write_to_clipboard(ClipboardItem::new_string(
                            self.source[range.clone()].to_owned(),
                        ));
                        if stroke.key.eq_ignore_ascii_case("x") {
                            self.source.replace_range(range.clone(), "");
                            self.cursor = range.start;
                            self.dirty = true;
                        }
                        self.selection = None;
                        self.selection_anchor = None;
                    }
                    return;
                }
                "v" => {
                    if let Some(text) = cx.read_from_clipboard().and_then(|item| item.text()) {
                        self.replace_selection(&text);
                    }
                    return;
                }
                _ => return,
            }
        }
        if stroke.modifiers.modified() {
            return;
        }

        match stroke.key.as_str() {
            "backspace" if self.selection.is_some() => self.replace_selection(""),
            "delete" if self.selection.is_some() => self.replace_selection(""),
            "backspace" if self.cursor > 0 => {
                let previous = self.source[..self.cursor]
                    .char_indices()
                    .next_back()
                    .map(|(index, _)| index)
                    .unwrap_or(0);
                self.source.replace_range(previous..self.cursor, "");
                self.cursor = previous;
                self.dirty = true;
            }
            "delete" if self.cursor < self.source.len() => {
                let next = self.source[self.cursor..]
                    .char_indices()
                    .nth(1)
                    .map(|(index, _)| self.cursor + index)
                    .unwrap_or(self.source.len());
                self.source.replace_range(self.cursor..next, "");
                self.dirty = true;
            }
            "left" if stroke.modifiers.shift => {
                self.move_cursor(self.previous_boundary(self.cursor), true);
            }
            "right" if stroke.modifiers.shift => {
                self.move_cursor(self.next_boundary(self.cursor), true);
            }
            "left" if self.cursor > 0 => {
                if let Some(range) = self.selection.take() {
                    self.cursor = range.start;
                } else {
                    self.cursor = self.previous_boundary(self.cursor);
                }
                self.selection_anchor = None;
            }
            "right" if self.cursor < self.source.len() => {
                if let Some(range) = self.selection.take() {
                    self.cursor = range.end;
                } else {
                    self.cursor = self.next_boundary(self.cursor);
                }
                self.selection_anchor = None;
            }
            "home" => {
                self.cursor = self.source[..self.cursor]
                    .rfind('\n')
                    .map(|index| index + 1)
                    .unwrap_or(0);
            }
            "end" => {
                self.cursor = self.source[self.cursor..]
                    .find('\n')
                    .map(|index| self.cursor + index)
                    .unwrap_or(self.source.len());
            }
            "enter" => self.insert_text("\n"),
            "tab" => self.insert_text("    "),
            _ => {
                if let Some(text) = stroke.key_char.as_deref() {
                    if !text.chars().any(char::is_control) {
                        self.insert_text(text);
                    }
                }
            }
        }
    }

    fn insert_text(&mut self, text: &str) {
        if self.selection.is_some() {
            self.replace_selection(text);
            return;
        }
        self.source.insert_str(self.cursor, text);
        self.cursor += text.len();
        self.dirty = true;
    }

    fn previous_boundary(&self, offset: usize) -> usize {
        self.source[..offset]
            .char_indices()
            .next_back()
            .map(|(index, _)| index)
            .unwrap_or(0)
    }

    fn next_boundary(&self, offset: usize) -> usize {
        self.source[offset..]
            .chars()
            .next()
            .map(|ch| offset + ch.len_utf8())
            .unwrap_or(self.source.len())
    }

    fn move_cursor(&mut self, offset: usize, extend: bool) {
        if extend {
            let anchor = *self.selection_anchor.get_or_insert(self.cursor);
            self.selection = Some(anchor.min(offset)..anchor.max(offset));
        } else {
            self.selection = None;
            self.selection_anchor = None;
        }
        self.cursor = offset;
    }

    fn replace_selection(&mut self, text: &str) {
        let range = self.selection.take().unwrap_or(self.cursor..self.cursor);
        self.source.replace_range(range.clone(), text);
        self.cursor = range.start + text.len();
        self.selection_anchor = None;
        self.dirty = true;
    }

    fn refresh_files(&mut self) {
        self.files = scan_project(&self.root);
        self.status = format!("Project refreshed · {} files indexed", self.files.len());
        self.output.push(self.status.clone());
    }

    fn build(&mut self, cx: &mut Context<Self>) {
        let Some(root) = self.example_build_root() else {
            self.status = "Building examples requires an Astra engine source checkout (set ASTRA_ENGINE_SOURCE_DIR)".into();
            self.output.push(self.status.clone());
            cx.notify();
            return;
        };
        let target = self.active_example.clone();
        let command_target = target.clone();
        let build_dir = [
            PathBuf::from("build"),
            PathBuf::from(".cache/build/debug"),
            PathBuf::from("cmake-build-debug"),
        ]
        .into_iter()
        .find(|path| root.join(path).join("CMakeCache.txt").is_file())
        .unwrap_or_else(|| PathBuf::from("build"));
        self.output.push(format!(
            "$ cmake --build {} --target {target} -j 4",
            build_dir.display()
        ));
        self.status = format!("Building {target}…");
        let executor = cx.background_executor().clone();
        cx.notify();
        cx.spawn(async move |this, cx| {
            let result = executor
                .spawn(async move {
                    Command::new("cmake")
                        .arg("--build")
                        .arg(build_dir)
                        .args(["--target", &command_target, "-j", "4"])
                        .current_dir(root)
                        .output()
                })
                .await;
            let _ = this.update(cx, |this: &mut Studio, cx: &mut Context<Studio>| {
                match result {
                    Ok(result) => {
                        let stdout = String::from_utf8_lossy(&result.stdout).to_string();
                        let stderr = String::from_utf8_lossy(&result.stderr).to_string();
                        let lines = stdout
                            .lines()
                            .chain(stderr.lines())
                            .map(str::to_owned)
                            .collect::<Vec<_>>();
                        this.output.extend(
                            lines
                                .into_iter()
                                .rev()
                                .take(70)
                                .collect::<Vec<_>>()
                                .into_iter()
                                .rev(),
                        );
                        this.status = if result.status.success() {
                            format!("Build succeeded · {target}")
                        } else {
                            format!("Build failed · exit {}", result.status.code().unwrap_or(-1))
                        };
                    }
                    Err(error) => this.status = format!("CMake could not start: {error}"),
                }
                this.output.push(this.status.clone());
                if this.output.len() > 100 {
                    this.output.drain(..this.output.len() - 100);
                }
                cx.notify();
            });
        })
        .detach();
    }

    fn run(&mut self) {
        let target = self.active_example.clone();
        let Some(root) = self.example_build_root() else {
            self.status = "Running examples requires an Astra engine build".into();
            self.output.push(self.status.clone());
            return;
        };
        let candidates = [
            root.join(format!("build/Debug/{target}")),
            root.join(format!("build/{target}")),
            root.join(format!("cmake-build-debug/{target}")),
            root.join(format!(".cache/build/debug/{target}")),
        ];
        if let Some(binary) = candidates.iter().find(|p| p.is_file()) {
            self.status = match Command::new(binary).current_dir(&root).spawn() {
                Ok(_) => "Runtime launched in its native window".into(),
                Err(e) => format!("Runtime launch failed: {e}"),
            };
        } else {
            self.status = format!("{target} is not built. Use Build first.");
        }
        self.output.push(self.status.clone());
    }
}

fn scan_project(root: &Path) -> Vec<PathBuf> {
    let mut pending = vec![root.to_path_buf()];
    let mut files = Vec::new();
    while let Some(dir) = pending.pop() {
        let Ok(entries) = fs::read_dir(dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
            if path.is_dir() {
                if !matches!(
                    name,
                    ".git" | "target" | "build" | ".cache" | "node_modules" | "vendor"
                ) {
                    pending.push(path);
                }
            } else if matches!(
                path.extension().and_then(|e| e.to_str()),
                Some(
                    "c" | "cc"
                        | "cpp"
                        | "hpp"
                        | "h"
                        | "hh"
                        | "mm"
                        | "metal"
                        | "hlsl"
                        | "glsl"
                        | "vert"
                        | "frag"
                        | "rs"
                        | "toml"
                        | "yaml"
                        | "yml"
                        | "xml"
                        | "js"
                        | "ts"
                        | "py"
                        | "sh"
                        | "glb"
                        | "gltf"
                        | "hdr"
                        | "png"
                        | "jpg"
                        | "jpeg"
                        | "json"
                        | "md"
                        | "cmake"
                )
            ) {
                files.push(path);
            }
        }
    }
    files.sort();
    files.truncate(2500);
    files
}

fn is_editable_source(path: &Path) -> bool {
    matches!(
        path.extension().and_then(|extension| extension.to_str()),
        Some(
            "c" | "cc"
                | "cpp"
                | "h"
                | "hh"
                | "hpp"
                | "hlsl"
                | "glsl"
                | "vert"
                | "frag"
                | "mm"
                | "metal"
                | "rs"
                | "json"
                | "md"
                | "txt"
                | "toml"
                | "yaml"
                | "yml"
                | "xml"
                | "js"
                | "ts"
                | "py"
                | "sh"
                | "cmake"
        )
    )
}

#[derive(Default)]
struct GltfMetadata {
    version: String,
    generator: String,
    scenes: usize,
    nodes: usize,
    meshes: usize,
    materials: usize,
    animations: usize,
}

fn glb_json_chunk(bytes: &[u8]) -> Result<String, String> {
    let read_u32 = |offset: usize| -> Result<u32, String> {
        let chunk = bytes
            .get(offset..offset + 4)
            .ok_or_else(|| "Truncated GLB header or chunk".to_owned())?;
        Ok(u32::from_le_bytes(
            chunk.try_into().expect("four-byte slice"),
        ))
    };
    if bytes.get(0..4) != Some(b"glTF") {
        return Err("Invalid GLB magic".into());
    }
    if read_u32(4)? != 2 {
        return Err("Only GLB 2.0 is supported".into());
    }
    if read_u32(8)? as usize != bytes.len() {
        return Err("GLB declared length does not match file size".into());
    }
    let mut offset = 12usize;
    let mut json = None;
    while offset < bytes.len() {
        let length = read_u32(offset)? as usize;
        let kind = read_u32(offset + 4)?;
        offset += 8;
        if length % 4 != 0 {
            return Err("GLB chunk length is not four-byte aligned".into());
        }
        let end = offset
            .checked_add(length)
            .filter(|end| *end <= bytes.len())
            .ok_or_else(|| "GLB chunk exceeds file size".to_owned())?;
        if kind == 0x4e4f534a {
            if offset != 20 || json.is_some() {
                return Err("GLB JSON chunk must be first and unique".into());
            }
            json = Some(
                String::from_utf8(bytes[offset..end].to_vec())
                    .map_err(|error| error.to_string())?
                    .trim_end_matches(['\0', ' '])
                    .to_owned(),
            );
        }
        offset = end;
    }
    json.ok_or_else(|| "GLB has no JSON chunk".into())
}

fn gltf_document_json(path: &Path) -> Result<String, String> {
    const MAX_ASSET_BYTES: u64 = 512 * 1024 * 1024;
    let metadata = fs::metadata(path).map_err(|error| error.to_string())?;
    if metadata.len() > MAX_ASSET_BYTES {
        return Err("Asset exceeds the engine's 512 MiB file limit".into());
    }
    let bytes = fs::read(path).map_err(|error| error.to_string())?;
    match path.extension().and_then(|extension| extension.to_str()) {
        Some("gltf") => String::from_utf8(bytes).map_err(|error| error.to_string()),
        Some("glb") => glb_json_chunk(&bytes),
        _ => Err("Unsupported asset format".into()),
    }
}

fn inspect_gltf(path: &Path) -> Result<GltfMetadata, String> {
    let json = gltf_document_json(path)?;
    parse_gltf_metadata(&json)
}

fn parse_gltf_metadata(json: &str) -> Result<GltfMetadata, String> {
    let document = serde_json::from_str::<serde_json::Value>(json)
        .map_err(|error| format!("Invalid glTF JSON: {error}"))?;
    let asset = document
        .get("asset")
        .ok_or_else(|| "Missing glTF asset metadata".to_owned())?;
    let version = asset
        .get("version")
        .and_then(serde_json::Value::as_str)
        .ok_or_else(|| "Missing glTF asset version".to_owned())?;
    if !version.starts_with("2.") {
        return Err("Astra's asset loader supports glTF 2.x documents".into());
    }
    let count = |key: &str| {
        document
            .get(key)
            .and_then(serde_json::Value::as_array)
            .map_or(0, Vec::len)
    };
    Ok(GltfMetadata {
        version: version.to_owned(),
        generator: asset
            .get("generator")
            .and_then(serde_json::Value::as_str)
            .unwrap_or("Unspecified")
            .to_owned(),
        scenes: count("scenes"),
        nodes: count("nodes"),
        meshes: count("meshes"),
        materials: count("materials"),
        animations: count("animations"),
    })
}

fn discover_learning_component(project_root: &Path) -> Option<LearningComponent> {
    let mut candidates = Vec::new();
    if let Some(path) = std::env::var_os("ASTRA_LEARNING_COMPONENT_DIR") {
        let root = PathBuf::from(path);
        candidates.push((root.clone(), root.join("manifest.json")));
    }
    candidates.push((
        project_root.to_path_buf(),
        project_root.join("learning/manifest.json"),
    ));
    if let Ok(executable) = std::env::current_exe() {
        if let Some(parent) = executable.parent() {
            for root in [
                parent.join("../share/astra/learning"),
                parent.join("share/astra/learning"),
                parent.join("../Resources/learning"),
            ] {
                candidates.push((root.clone(), root.join("manifest.json")));
            }
        }
    }

    for (content_root, manifest_path) in candidates {
        let Ok(root) = content_root.canonicalize() else {
            continue;
        };
        let Ok(manifest_path) = manifest_path.canonicalize() else {
            continue;
        };
        if !manifest_path.starts_with(&root) {
            continue;
        }
        let Ok(json) = fs::read_to_string(manifest_path) else {
            continue;
        };
        let Ok(manifest) = serde_json::from_str::<LearningManifest>(&json) else {
            continue;
        };
        if manifest.format != "astra-learning-component" || manifest.version != 1 {
            continue;
        }
        let examples = manifest
            .examples
            .into_iter()
            .filter_map(|mut example| {
                if !component_file_exists(&root, &example.source) {
                    return None;
                }
                if example
                    .guide
                    .as_ref()
                    .is_some_and(|guide| !component_file_exists(&root, guide))
                {
                    example.guide = None;
                }
                Some(example)
            })
            .collect::<Vec<_>>();
        if !examples.is_empty() {
            return Some(LearningComponent { root, examples });
        }
    }
    None
}

fn component_file_exists(root: &Path, relative: &str) -> bool {
    let path = root.join(relative);
    path.canonicalize()
        .is_ok_and(|resolved| resolved.starts_with(root) && resolved.is_file())
}

fn label(text: impl Into<String>, color: u32, size: f32) -> gpui::Div {
    div()
        .text_size(px(size))
        .text_color(rgb(color))
        .child(text.into())
}
fn eyebrow(text: &str) -> gpui::Div {
    div()
        .text_size(px(9.))
        .font_weight(gpui::FontWeight::SEMIBOLD)
        .text_color(rgb(MUTED))
        .child(text.to_uppercase())
}
fn action(text: &str, active: bool) -> gpui::Div {
    div()
        .px_2()
        .py(px(5.))
        .cursor_pointer()
        .text_size(px(11.))
        .font_weight(gpui::FontWeight::MEDIUM)
        .bg(rgb(if active { 0x414141 } else { RAISED }))
        .text_color(rgb(if active { 0xf0f0f0 } else { 0xbcbcbc }))
        .border_1()
        .border_color(rgb(if active { 0x585858 } else { BORDER }))
        .hover(|s| s.bg(rgb(0x484848)))
        .child(text.to_string())
}
fn nav_item(glyph: &str, title: &str, active: bool) -> gpui::Div {
    div()
        .mx_2()
        .h(px(34.))
        .px_3()
        .flex()
        .items_center()
        .gap_3()
        .cursor_pointer()
        .bg(rgb(if active { 0x343434 } else { 0x1d1d1d }))
        .text_color(rgb(if active { TEXT } else { MUTED }))
        .child(label(glyph, if active { TEXT } else { MUTED }, 14.))
        .child(label(title, if active { TEXT } else { 0xb0b0b0 }, 11.))
}
fn panel_heading(name: &str, trailing: &str) -> gpui::Div {
    div()
        .h(px(32.))
        .px_3()
        .flex()
        .items_center()
        .justify_between()
        .bg(rgb(PANEL))
        .border_b_1()
        .border_color(rgb(BORDER))
        .child(eyebrow(name))
        .child(label(trailing, DIM, 11.))
}
impl Render for Studio {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if self.page != "editor" {
            let project_name = self.project.name.clone();
            let project_path = self.root.display().to_string();
            let learning_available = self.learning.is_some();
            let examples = self
                .learning
                .as_ref()
                .map(|component| component.examples.clone())
                .unwrap_or_default();
            let can_build_examples = self.example_build_root().is_some_and(|root| {
                ["build", ".cache/build/debug", "cmake-build-debug"]
                    .iter()
                    .any(|path| root.join(path).join("CMakeCache.txt").is_file())
            });
            let titlebar = div()
                .h(px(38.))
                .w_full()
                .pl(px(82.))
                .pr_4()
                .flex()
                .items_center()
                .justify_between()
                .bg(rgb(0x191919))
                .border_b_1()
                .border_color(rgb(0x303030))
                .window_control_area(WindowControlArea::Drag)
                .child(label("Astra Studio", 0xbdbdbd, 10.))
                .child(label("WELCOME", DIM, 8.));
            let sidebar = div()
                .w(px(218.))
                .h_full()
                .flex()
                .flex_col()
                .bg(rgb(0x1d1d1d))
                .border_r_1()
                .border_color(rgb(BORDER))
                .child(
                    div()
                        .h(px(72.))
                        .px_5()
                        .flex()
                        .items_center()
                        .gap_3()
                        .border_b_1()
                        .border_color(rgb(BORDER))
                        .child(label("A", 0xe0e0e0, 25.))
                        .child(
                            div()
                                .flex()
                                .flex_col()
                                .gap_1()
                                .child(label("ASTRA", TEXT, 12.))
                                .child(label("STUDIO", MUTED, 8.)),
                        ),
                )
                .child(div().px_3().pt_4().pb_2().child(eyebrow("Workspace")))
                .child(
                    div()
                        .id("nav-home")
                        .on_click(cx.listener(|this, _, _, cx| {
                            this.page = "home".into();
                            cx.notify();
                        }))
                        .child(nav_item("⌂", "Home", self.page == "home")),
                )
                .when(learning_available, |sidebar| {
                    sidebar.child(
                        div()
                            .id("nav-learn")
                            .on_click(cx.listener(|this, _, _, cx| {
                                if this.learning.is_some() {
                                    this.page = "learn".into();
                                }
                                cx.notify();
                            }))
                            .child(nav_item("◇", "Learn", self.page == "learn")),
                    )
                })
                .child(div().flex_1())
                .child(
                    div()
                        .px_4()
                        .py_3()
                        .border_t_1()
                        .border_color(rgb(BORDER))
                        .child(label("ASTRA ENGINE  ·  0.1", DIM, 8.)),
                );

            let content = if self.page == "learn" && learning_available {
                div()
                    .id("learn-content")
                    .flex_1()
                    .h_full()
                    .min_h(px(0.))
                    .flex()
                    .flex_col()
                    .bg(rgb(BG))
                    .child(
                        div()
                            .px_8()
                            .pt_8()
                            .pb_6()
                            .border_b_1()
                            .border_color(rgb(BORDER))
                            .child(eyebrow("Documentation & examples"))
                            .child(div().mt_2().child(label("Learn Astra", 0xe0e0e0, 27.)))
                            .child(div().mt_2().child(label(
                                "Explore working examples of the engine, from core systems to a streamed open world.",
                                MUTED,
                                11.,
                            )))
                            .child(div().mt_3().child(label(
                                format!("{} EXAMPLES  ·  OPTIONAL CONTENT", examples.len()),
                                DIM,
                                8.,
                            ))),
                    )
                    .child(
                        div()
                            .id("examples-scroll")
                            .flex_1()
                            .h_full()
                            .min_h(px(0.))
                            .overflow_y_scroll()
                            .px_8()
                            .py_5()
                            .flex()
                            .flex_col()
                            .gap_2()
                            .children(examples.iter().enumerate().map(|(index, example)| {
                                let target = example.target.clone();
                                let source_target = target.clone();
                                let source = example.source.clone();
                                let guide = example.guide.clone();
                                div()
                                    .flex()
                                    .items_center()
                                    .justify_between()
                                    .gap_4()
                                    .p_4()
                                    .bg(rgb(SURFACE))
                                    .border_1()
                                    .border_color(rgb(BORDER))
                                    .child(
                                        div()
                                            .flex_1()
                                            .flex()
                                            .flex_col()
                                            .gap_1()
                                            .child(label(example.title.clone(), TEXT, 12.))
                                            .child(label(example.description.clone(), MUTED, 10.))
                                            .child(label(
                                                format!("{}  ·  {}", example.platform, example.source),
                                                DIM,
                                                8.,
                                            )),
                                    )
                                    .child(
                                        div()
                                            .flex()
                                            .items_center()
                                            .gap_2()
                                            .when(guide.is_some(), |actions| {
                                                let guide = guide.clone().unwrap();
                                                actions.child(
                                                    div()
                                                        .id(("example-guide", index))
                                                        .on_click(cx.listener(move |this, _, _, cx| {
                                                            if let Some(component) = &this.learning {
                                                                let path = component.root.join(&guide);
                                                                match fs::read_to_string(&path) {
                                                                    Ok(text) => {
                                                                        this.document = Some(path.clone());
                                                                        this.selected = Some(path);
                                                                        this.source = text;
                                                                        this.cursor = 0;
                                                                        this.dirty = false;
                                                                        this.page = "editor".into();
                                                                    }
                                                                    Err(error) => this.status = format!("Could not open guide: {error}"),
                                                                }
                                                            }
                                                            cx.notify();
                                                        }))
                                                        .child(action("Guide", false)),
                                                )
                                            })
                                            .child(
                                                div()
                                                    .id(("example-source", index))
                                                    .on_click(cx.listener(move |this, _, _, cx| {
                                                        this.open_example_source(&source, &source_target);
                                                        cx.notify();
                                                    }))
                                                    .child(action("View source", false)),
                                            )
                                            .when(can_build_examples, |actions| {
                                                let build_target = target.clone();
                                                actions.child(
                                                    div()
                                                        .id(("example-build", index))
                                                        .on_click(cx.listener(move |this, _, _, cx| {
                                                            this.active_example = build_target.clone();
                                                            this.page = "editor".into();
                                                            this.build(cx);
                                                        }))
                                                        .child(action("Build", true)),
                                                )
                                            }),
                                    )
                            })),
                    )
            } else {
                div()
                    .id("home-content-scroll")
                    .flex_1()
                    .h_full()
                    .min_h(px(0.))
                    .overflow_y_scroll()
                    .flex()
                    .flex_col()
                    .bg(rgb(BG))
                    .child(
                        div()
                            .px_8()
                            .pt_9()
                            .pb_7()
                            .child(eyebrow("Game development workspace"))
                            .child(div().mt_2().child(label(
                                "Welcome to Astra Studio",
                                0xe4e4e4,
                                28.,
                            )))
                            .child(div().mt_2().child(label(
                                "Build worlds, explore engine systems, and create with Astra.",
                                MUTED,
                                11.,
                            ))),
                    )
                    .child(
                        div()
                            .mx_8()
                            .p_5()
                            .bg(rgb(SURFACE))
                            .border_1()
                            .border_color(rgb(BORDER))
                            .child(eyebrow("Current project"))
                            .child(div().mt_3().child(label(project_name, 0xe0e0e0, 19.)))
                            .child(div().mt_1().child(label(project_path.clone(), MUTED, 9.)))
                            .child(
                                div()
                                    .mt_4()
                                    .flex()
                                    .items_center()
                                    .gap_2()
                                    .child(
                                        div()
                                            .id("continue-project")
                                            .on_click(cx.listener(|this, _, _, cx| {
                                                this.page = "editor".into();
                                                cx.notify();
                                            }))
                                            .child(action("Continue working", true)),
                                    )
                                    .child(
                                        div()
                                            .id("open-project")
                                            .on_click(cx.listener(|this, _, _, cx| {
                                                this.choose_project(cx);
                                            }))
                                            .child(action("Open project…", false)),
                                    ),
                            ),
                    )
                    .child(div().px_8().pt_8().pb_3().child(eyebrow("Get started")))
                    .child(
                        div()
                            .px_8()
                            .flex()
                            .gap_3()
                            .when(learning_available, |row| {
                                row.child(
                                div()
                                    .flex_1()
                                    .p_4()
                                    .bg(rgb(SURFACE))
                                    .border_1()
                                    .border_color(rgb(BORDER))
                                    .child(label("Learn the engine", TEXT, 12.))
                                    .child(div().mt_2().child(label(
                                        "Browse practical examples and inspect their source code.",
                                        MUTED,
                                        10.,
                                    )))
                                    .child(
                                        div()
                                            .mt_4()
                                            .id("home-learn")
                                            .on_click(cx.listener(|this, _, _, cx| {
                                                this.page = "learn".into();
                                                cx.notify();
                                            }))
                                            .child(action("Browse examples  →", false)),
                                    ),
                            )
                            })
                            .child(
                                div()
                                    .flex_1()
                                    .p_4()
                                    .bg(rgb(SURFACE))
                                    .border_1()
                                    .border_color(rgb(BORDER))
                                    .child(label("Open the editor", TEXT, 12.))
                                    .child(div().mt_2().child(label(
                                        "Work with project files, scenes, and build output.",
                                        MUTED,
                                        10.,
                                    )))
                                    .child(
                                        div()
                                            .mt_4()
                                            .id("home-editor")
                                            .on_click(cx.listener(|this, _, _, cx| {
                                                this.page = "editor".into();
                                                cx.notify();
                                            }))
                                            .child(action("Open workspace  →", false)),
                                    ),
                            ),
                    )
                    .child(div().flex_1())
                    .child(div().px_8().pb_5().child(label(
                        format!("{} PROJECT FILES INDEXED", self.files.len()),
                        DIM,
                        8.,
                    )))
            };

            return div()
                .size_full()
                .flex()
                .flex_col()
                .bg(rgb(BG))
                .child(titlebar)
                .child(
                    div()
                        .flex_1()
                        .h_full()
                        .min_h(px(0.))
                        .flex()
                        .child(sidebar)
                        .child(content),
                );
        }

        let project_name = self.project.name.clone();
        let selected_path = self.selected.clone();
        let document_path = self.document.clone();
        let document_name = document_path
            .as_ref()
            .map(|p| {
                p.strip_prefix(&self.root)
                    .unwrap_or(p)
                    .display()
                    .to_string()
            })
            .unwrap_or_else(|| "Untitled".into());
        let short_document = document_path
            .as_ref()
            .and_then(|p| p.file_name())
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_else(|| "Untitled".into());

        let toolbar = div()
            .h(px(48.))
            .px_3()
            .flex()
            .items_center()
            .justify_between()
            .bg(rgb(0x222222))
            .border_b_1()
            .border_color(rgb(BORDER))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_3()
                    .child(
                        div()
                            .id("editor-home")
                            .on_click(cx.listener(|this, _, _, cx| {
                                if this.request_navigation(PendingNavigation::Home) {
                                    this.page = "home".into();
                                }
                                cx.notify();
                            }))
                            .child(action("⌂  Home", false)),
                    )
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap_2()
                            .child(label("A", 0xe5e5e5, 18.))
                            .child(
                                div()
                                    .flex()
                                    .flex_col()
                                    .gap(px(0.))
                                    .child(label("ASTRA", TEXT, 11.))
                                    .child(label("STUDIO", MUTED, 8.)),
                            ),
                    )
                    .child(div().w(px(1.)).h(px(25.)).bg(rgb(BORDER)))
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(1.))
                            .child(label(project_name.clone(), TEXT, 11.))
                            .child(label("GAME PROJECT  /  WORKSPACE", DIM, 8.)),
                    )
                    .child(div().w(px(1.)).h(px(25.)).bg(rgb(BORDER)))
                    .child(
                        div().flex().items_center().gap_1().children(
                            [("3D", "3d"), ("Design", "design"), ("Code", "code")]
                                .into_iter()
                                .map(|(title, workspace)| {
                                    let active = self.workspace == workspace;
                                    div()
                                        .id(title)
                                        .on_click(cx.listener(move |this, _, _, cx| {
                                            this.workspace = workspace.into();
                                            this.selected = if workspace == "code" {
                                                this.document.clone()
                                            } else {
                                                None
                                            };
                                            cx.notify();
                                        }))
                                        .child(action(title, active))
                                }),
                        ),
                    ),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_2()
                    .child(
                        div()
                            .id("open-selected")
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.open_selected();
                                cx.notify();
                            }))
                            .child(action("Open", false)),
                    )
                    .child(
                        div()
                            .id("refresh-project")
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.refresh_files();
                                cx.notify();
                            }))
                            .child(action("Refresh", false)),
                    )
                    .when(self.workspace == "code", |bar| {
                        bar.child(
                            div()
                                .id("save-document")
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.save_document();
                                    cx.notify();
                                }))
                                .child(action(
                                    if self.dirty { "Save •" } else { "Save" },
                                    self.dirty,
                                )),
                        )
                        .child(
                            div()
                                .id("build")
                                .on_click(cx.listener(|this, _, _, cx| this.build(cx)))
                                .child(action("Build", false)),
                        )
                        .child(
                            div()
                                .id("run")
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.run();
                                    cx.notify();
                                }))
                                .child(action("▶  Play", true)),
                        )
                    })
                    .child(div().w(px(1.)).h(px(22.)).bg(rgb(BORDER))),
            );

        let files = self.files.iter().take(38).cloned().collect::<Vec<_>>();
        let assets = self
            .files
            .iter()
            .filter(|path| {
                matches!(
                    path.extension().and_then(|ext| ext.to_str()),
                    Some("hdr" | "png" | "jpg" | "jpeg")
                )
            })
            .cloned()
            .collect::<Vec<_>>();
        let gltf_assets = self
            .files
            .iter()
            .filter(|path| {
                matches!(
                    path.extension().and_then(|extension| extension.to_str()),
                    Some("gltf" | "glb")
                )
            })
            .cloned()
            .collect::<Vec<_>>();
        let browser_paths = match self.workspace.as_str() {
            "3d" => gltf_assets.clone(),
            "design" => assets.clone(),
            _ => files.clone(),
        };
        let browser_title = match self.workspace.as_str() {
            "3d" => "glTF / GLB ASSETS",
            "design" => "DESIGN ASSETS",
            _ => "PROJECT FILES",
        };
        let browser_is_empty = browser_paths.is_empty();
        let browser_count = browser_paths.len();
        let browser_list = div()
            .id("asset-browser-scroll")
            .flex_1()
            .h_full()
            .min_h(px(0.))
            .overflow_y_scroll()
            .children(browser_paths.into_iter().enumerate().map(|(i, path)| {
                let rel = path
                    .strip_prefix(&self.root)
                    .unwrap_or(&path)
                    .display()
                    .to_string();
                let name = path
                    .file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or(rel);
                let chosen = selected_path.as_ref() == Some(&path);
                let target = path.clone();
                let extension = path
                    .extension()
                    .and_then(|extension| extension.to_str())
                    .unwrap_or("")
                    .to_uppercase();
                div()
                    .id(("file", i))
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.selected = Some(target.clone());
                        this.status = format!("Selected {}", target.display());
                        cx.notify();
                    }))
                    .h(px(26.))
                    .px_3()
                    .flex()
                    .items_center()
                    .gap_2()
                    .bg(rgb(if chosen { SELECTED } else { PANEL }))
                    .child(label(
                        if extension == "GLB" { "◇" } else { "·" },
                        MUTED,
                        12.,
                    ))
                    .child(div().flex_1().child(label(
                        name,
                        if chosen { TEXT } else { 0xa9a9a9 },
                        10.,
                    )))
                    .child(label(extension, DIM, 8.))
            }))
            .when(browser_is_empty, |list| {
                list.child(div().p_3().child(label(
                    if self.workspace == "3d" {
                        "No .gltf or .glb assets found in this project."
                    } else if self.workspace == "design" {
                        "No supported design assets found in this project."
                    } else {
                        "No project source files found."
                    },
                    MUTED,
                    10.,
                )))
            });
        let explorer = div()
            .w(px(218.))
            .min_w(px(175.))
            .flex()
            .flex_col()
            .bg(rgb(PANEL))
            .border_r_1()
            .border_color(rgb(BORDER))
            .child(
                div()
                    .h(px(32.))
                    .px_3()
                    .flex()
                    .items_center()
                    .justify_between()
                    .bg(rgb(PANEL))
                    .border_b_1()
                    .border_color(rgb(BORDER))
                    .child(eyebrow(browser_title))
                    .child(label(format!("{} ITEMS", browser_count), DIM, 8.)),
            )
            .child(browser_list);

        let selected_title = selected_path
            .as_ref()
            .map(|p| {
                p.file_name()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .to_string()
            })
            .unwrap_or_else(|| "No selection".into());
        let selected_gltf = selected_path.as_ref().filter(|path| {
            matches!(
                path.extension().and_then(|extension| extension.to_str()),
                Some("gltf" | "glb")
            )
        });
        let gltf_inspection = selected_gltf.map(|path| inspect_gltf(path.as_path()));
        let gltf_metadata = gltf_inspection
            .as_ref()
            .and_then(|result| result.as_ref().ok());
        let gltf_error = gltf_inspection
            .as_ref()
            .and_then(|result| result.as_ref().err())
            .cloned()
            .unwrap_or_default();
        let file_metadata = selected_path
            .as_ref()
            .and_then(|path| fs::metadata(path).ok());
        let metadata_rows = if self.workspace == "3d" {
            vec![
                (
                    "Format",
                    selected_gltf.map_or("—".into(), |path| {
                        path.extension()
                            .unwrap_or_default()
                            .to_string_lossy()
                            .to_uppercase()
                    }),
                ),
                (
                    "File size",
                    file_metadata
                        .as_ref()
                        .map_or("—".into(), |metadata| format!("{} bytes", metadata.len())),
                ),
                (
                    "glTF version",
                    gltf_metadata.map_or("—".into(), |metadata| metadata.version.clone()),
                ),
                (
                    "Scenes",
                    gltf_metadata.map_or("—".into(), |metadata| metadata.scenes.to_string()),
                ),
                (
                    "Nodes",
                    gltf_metadata.map_or("—".into(), |metadata| metadata.nodes.to_string()),
                ),
                (
                    "Meshes",
                    gltf_metadata.map_or("—".into(), |metadata| metadata.meshes.to_string()),
                ),
                (
                    "Materials",
                    gltf_metadata.map_or("—".into(), |metadata| metadata.materials.to_string()),
                ),
                (
                    "Animations",
                    gltf_metadata.map_or("—".into(), |metadata| metadata.animations.to_string()),
                ),
                (
                    "Generator",
                    gltf_metadata.map_or("—".into(), |metadata| metadata.generator.clone()),
                ),
            ]
        } else {
            vec![
                (
                    "Format",
                    selected_path
                        .as_ref()
                        .and_then(|path| path.extension())
                        .map_or("—".into(), |extension| {
                            extension.to_string_lossy().to_uppercase()
                        }),
                ),
                (
                    "File size",
                    file_metadata
                        .as_ref()
                        .map_or("—".into(), |metadata| format!("{} bytes", metadata.len())),
                ),
            ]
        };
        let inspector = div()
            .w(px(252.))
            .min_w(px(205.))
            .flex()
            .flex_col()
            .bg(rgb(PANEL))
            .border_l_1()
            .border_color(rgb(BORDER))
            .child(panel_heading(
                "Inspector",
                if self.workspace == "3d" {
                    "glTF asset metadata"
                } else {
                    "Selected file"
                },
            ))
            .child(
                div()
                    .px_3()
                    .py_3()
                    .border_b_1()
                    .border_color(rgb(BORDER))
                    .flex()
                    .items_center()
                    .gap_2()
                    .child(
                        div()
                            .size(px(28.))
                            .flex()
                            .items_center()
                            .justify_center()
                            .bg(rgb(RAISED))
                            .border_1()
                            .border_color(rgb(BORDER))
                            .child(label("☼", 0xc7c7c7, 15.)),
                    )
                    .child(
                        div()
                            .flex_1()
                            .flex()
                            .flex_col()
                            .gap(px(2.))
                            .child(label(selected_title.clone(), TEXT, 11.))
                            .child(label("SELECTED ASSET", DIM, 8.)),
                    )
                    .child(label("◇", MUTED, 11.)),
            )
            .child(
                div()
                    .px_3()
                    .py_2()
                    .flex()
                    .items_center()
                    .justify_between()
                    .child(eyebrow("Asset metadata")),
            )
            .children(metadata_rows.into_iter().map(|(key, value)| {
                div()
                    .px_3()
                    .py_1()
                    .flex()
                    .items_start()
                    .gap_2()
                    .child(div().w(px(76.)).child(label(key, MUTED, 9.)))
                    .child(div().flex_1().child(label(value, 0xc4c4c4, 9.)))
            }))
            .when(!gltf_error.is_empty(), |panel| {
                panel.child(div().px_3().py_2().child(label(gltf_error, 0xc99a82, 9.)))
            })
            .child(div().h(px(1.)).mx_3().my_2().bg(rgb(BORDER)))
            .child(
                div()
                    .px_3()
                    .py_2()
                    .flex()
                    .items_center()
                    .justify_between()
                    .child(eyebrow("Location"))
                    .child(label("⌄", MUTED, 11.)),
            )
            .child(
                div()
                    .px_3()
                    .pb_3()
                    .child(label(selected_title.clone(), 0xbdbdbd, 10.))
                    .child(label(
                        selected_path.as_ref().map_or_else(
                            || "No file selected".into(),
                            |path| path.display().to_string(),
                        ),
                        DIM,
                        9.,
                    )),
            )
            .child(div().flex_1());

        let viewport = if self.workspace == "design" {
            div()
                .flex_1()
                .min_w(px(280.))
                .flex()
                .flex_col()
                .bg(rgb(BG))
                .child(panel_heading("Design assets", "Project library"))
                .child(
                    div()
                        .id("design-assets-scroll")
                        .flex_1()
                        .overflow_y_scroll()
                        .p_4()
                        .flex()
                        .flex_col()
                        .gap_2()
                        .children(assets.iter().enumerate().map(|(index, path)| {
                            let target = path.clone();
                            let name = path
                                .file_name()
                                .unwrap_or_default()
                                .to_string_lossy()
                                .to_string();
                            let selected = selected_path.as_ref() == Some(path);
                            div()
                                .id(("design-asset", index))
                                .on_click(cx.listener(move |this, _, _, cx| {
                                    this.selected = Some(target.clone());
                                    this.status = format!("Selected asset {}", target.display());
                                    cx.notify();
                                }))
                                .px_3()
                                .py_3()
                                .bg(rgb(if selected { SELECTED } else { SURFACE }))
                                .border_1()
                                .border_color(rgb(BORDER))
                                .flex()
                                .items_center()
                                .justify_between()
                                .child(label(name, if selected { TEXT } else { 0xbcbcbc }, 11.))
                                .child(label(
                                    path.extension()
                                        .and_then(|ext| ext.to_str())
                                        .unwrap_or("file")
                                        .to_uppercase(),
                                    DIM,
                                    8.,
                                ))
                        }))
                        .when(assets.is_empty(), |list| {
                            list.child(label(
                                "No supported design assets found in this project.",
                                MUTED,
                                11.,
                            ))
                        }),
                )
                .child(
                    div()
                        .p_3()
                        .border_t_1()
                        .border_color(rgb(BORDER))
                        .flex()
                        .items_center()
                        .justify_between()
                        .child(label(selected_title.clone(), MUTED, 10.))
                        .child(
                            div()
                                .id("design-open-asset")
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.open_selected();
                                    cx.notify();
                                }))
                                .child(action("Open asset", true)),
                        ),
                )
        } else if self.workspace == "3d" {
            div()
                .flex_1()
                .min_w(px(280.))
                .flex()
                .flex_col()
                .bg(rgb(BG))
                .child(panel_heading("3D assets", "glTF 2.0 · GLB 2.0"))
                .child(
                    div()
                        .flex_1()
                        .p_6()
                        .flex()
                        .flex_col()
                        .gap_3()
                        .child(label(selected_title.clone(), 0xe0e0e0, 19.))
                        .child(label(
                            "Browse glTF 2.0 and binary GLB 2.0 assets supported by Astra's asset pipeline. Select an asset to inspect its document metadata, then open it in the default external application.",
                            MUTED,
                            11.,
                        ))
                        .when(selected_gltf.is_some(), |content| {
                            content.child(
                                div()
                                    .mt_2()
                                    .id("open-gltf-externally")
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.open_selected();
                                        cx.notify();
                                    }))
                                    .child(action("Open asset externally", true)),
                            )
                        })
                        .when(selected_gltf.is_none(), |content| {
                            content.child(label(
                                format!("{} glTF / GLB assets indexed", gltf_assets.len()),
                                TEXT,
                                12.,
                            ))
                        })
                )
        } else {
            div()
                .flex_1()
                .min_w(px(280.))
                .flex()
                .flex_col()
                .bg(rgb(BG))
                .child(panel_heading("Code workspace", "Source editor"))
                .child(
                    div()
                        .flex_1()
                        .p_6()
                        .flex()
                        .flex_col()
                        .gap_2()
                        .child(label(short_document.clone(), 0xe0e0e0, 19.))
                        .child(label(
                            document_path
                                .as_ref()
                                .map(|path| path.display().to_string())
                                .unwrap_or_else(|| "Choose a source file from Project files".into()),
                            MUTED,
                            10.,
                        ))
                        .child(label(
                            "Edit the document in the source pane below. Use ⌘S to save your changes.",
                            MUTED,
                            11.,
                        )),
                )
        };

        let tabs = div()
            .h(px(34.))
            .flex()
            .items_center()
            .bg(rgb(0x222222))
            .border_b_1()
            .border_color(rgb(BORDER))
            .child(
                div()
                    .h_full()
                    .px_3()
                    .flex()
                    .items_center()
                    .gap_2()
                    .bg(rgb(BG))
                    .border_t_1()
                    .border_color(rgb(0xb0b0b0))
                    .child(label("◈", MUTED, 10.))
                    .child(label(short_document.clone(), TEXT, 10.))
                    .child(label(if self.dirty { "●" } else { "" }, MUTED, 8.)),
            )
            .child(div().flex_1())
            .child(div().px_3().child(label("SOURCE", DIM, 8.)));

        let cursor_line = self.source[..self.cursor].matches('\n').count();
        let line_start = self.source[..self.cursor]
            .rfind('\n')
            .map(|index| index + 1)
            .unwrap_or(0);
        let cursor_column = self.source[line_start..self.cursor].chars().count();
        let selection = self.selection.clone();
        let code_lines = self
            .source
            .split('\n')
            .enumerate()
            .scan(0usize, |offset, (i, line)| {
                let line_offset = *offset;
                *offset += line.len() + 1;
                Some((i, line, line_offset))
            })
            .map(|(i, line, line_offset)| {
                let line_selected = selection.as_ref().is_some_and(|range| {
                    range.start < line_offset + line.len() && range.end > line_offset
                });
                let trimmed = line.trim_start();
                let color = if trimmed.starts_with("//") || trimmed.starts_with('#') {
                    0x777777
                } else if trimmed.starts_with("class ")
                    || trimmed.starts_with("struct ")
                    || trimmed.starts_with("namespace ")
                {
                    0xd0d0d0
                } else {
                    0xb8b8b8
                };
                let display_line = if i == cursor_line {
                    let mut chars = line.chars();
                    let before = chars.by_ref().take(cursor_column).collect::<String>();
                    format!("{before}▏{}", chars.collect::<String>())
                } else {
                    line.to_owned()
                };
                div()
                    .flex()
                    .gap_3()
                    .px_3()
                    .py(px(1.))
                    .bg(rgb(if line_selected { 0x343434 } else { BG }))
                    .hover(|s| s.bg(rgb(0x242424)))
                    .child(
                        div()
                            .w(px(30.))
                            .text_right()
                            .text_size(px(10.))
                            .text_color(rgb(DIM))
                            .child((i + 1).to_string()),
                    )
                    .child(
                        div()
                            .text_size(px(10.))
                            .font_family("SF Mono")
                            .text_color(rgb(color))
                            .child(if display_line.is_empty() {
                                " ".into()
                            } else {
                                display_line
                            }),
                    )
            })
            .collect::<Vec<_>>();
        let line_number = self.source[..self.cursor]
            .bytes()
            .filter(|byte| *byte == b'\n')
            .count()
            + 1;
        let language = document_path
            .as_ref()
            .and_then(|path| path.extension())
            .map(|extension| extension.to_string_lossy().to_uppercase())
            .unwrap_or_else(|| "TEXT".into());
        let source_editor = div()
            .flex()
            .flex_col()
            .bg(rgb(BG))
            .border_t_1()
            .border_color(rgb(BORDER))
            .id("source-editor")
            .track_focus(&self.editor_focus)
            .on_click(cx.listener(|this, _, window, _| {
                window.focus(&this.editor_focus);
            }))
            .on_key_down(cx.listener(|this, event, _, cx| {
                this.edit_key(event, cx);
                cx.notify();
            }))
            .child(
                div()
                    .h(px(27.))
                    .px_3()
                    .flex()
                    .items_center()
                    .justify_between()
                    .bg(rgb(SURFACE))
                    .child(label(format!("{}  /  SOURCE", document_name), MUTED, 9.))
                    .child(label(
                        if self.dirty {
                            "MODIFIED · ⌘S TO SAVE"
                        } else {
                            "EDITABLE"
                        },
                        if self.dirty { 0xd0b078 } else { DIM },
                        8.,
                    )),
            )
            .child(
                div()
                    .id("source-editor-content")
                    .h(px(if self.workspace == "code" { 360. } else { 190. }))
                    .overflow_y_scroll()
                    .py_1()
                    .children(code_lines),
            );

        let output = self
            .output
            .iter()
            .rev()
            .take(5)
            .cloned()
            .collect::<Vec<_>>();
        let console = div()
            .h(px(154.))
            .flex()
            .flex_col()
            .bg(rgb(0x1b1b1b))
            .border_t_1()
            .border_color(rgb(BORDER))
            .child(
                div()
                    .h(px(31.))
                    .px_3()
                    .flex()
                    .items_center()
                    .justify_between()
                    .bg(rgb(SURFACE))
                    .child(eyebrow("Output"))
                    .child(
                        div()
                            .id("clear-output")
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.output.clear();
                                cx.notify();
                            }))
                            .child(label("CLEAR  ×", MUTED, 8.)),
                    ),
            )
            .child(
                div()
                    .flex_1()
                    .overflow_hidden()
                    .px_3()
                    .py_2()
                    .flex()
                    .flex_col()
                    .gap_1()
                    .children(output.into_iter().rev().map(|line| {
                        div()
                            .text_size(px(9.))
                            .font_family("SF Mono")
                            .text_color(rgb(0xb8b8b8))
                            .child(line)
                    })),
            );

        let footer = div()
            .h(px(23.))
            .px_3()
            .flex()
            .items_center()
            .justify_between()
            .bg(rgb(0x222222))
            .border_t_1()
            .border_color(rgb(BORDER))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_3()
                    .child(label("●", 0xaaaaaa, 7.))
                    .child(label(self.status.clone(), MUTED, 9.)),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_3()
                    .child(label(language, MUTED, 8.))
                    .child(label(format!("Ln {line_number}"), MUTED, 8.))
                    .child(label("UTF-8", MUTED, 8.)),
            );

        let titlebar = div()
            .h(px(38.))
            .w_full()
            .px_4()
            .pl(px(82.))
            .flex()
            .items_center()
            .justify_between()
            .bg(rgb(0x191919))
            .border_b_1()
            .border_color(rgb(0x303030))
            .window_control_area(WindowControlArea::Drag)
            .child(label("Astra Studio", 0xbdbdbd, 10.))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_2()
                    .child(label(project_name, 0x9a9a9a, 9.))
                    .child(label("/", DIM, 9.))
                    .child(label(short_document, 0x777777, 9.)),
            );

        let root = div()
            .size_full()
            .flex()
            .flex_col()
            .bg(rgb(BG))
            .text_color(rgb(TEXT))
            .on_action(cx.listener(|this, _: &OpenProject, _, cx| this.choose_project(cx)))
            .on_action(cx.listener(|this, _: &SaveDocument, _, cx| {
                this.save_document();
                cx.notify();
            }))
            .on_action(cx.listener(|this, _: &RefreshProject, _, cx| {
                this.refresh_files();
                cx.notify();
            }))
            .on_action(cx.listener(|this, _: &BuildProject, _, cx| this.build(cx)))
            .on_action(cx.listener(|this, _: &RunProject, _, cx| {
                this.run();
                cx.notify();
            }))
            .on_action(cx.listener(|this, _: &GoHome, _, cx| {
                if this.request_navigation(PendingNavigation::Home) {
                    this.page = "home".into();
                }
                cx.notify();
            }))
            .on_action(cx.listener(|this, _: &ShowLearn, _, cx| {
                if this.learning.is_some() {
                    this.page = "learn".into();
                }
                cx.notify();
            }))
            .on_action(cx.listener(|this, _: &ShowAbout, _, cx| {
                this.status = "Astra Studio 0.1.0 · Native GPUI editor".into();
                this.output.push(this.status.clone());
                cx.notify();
            }))
            .on_action(cx.listener(|this, _: &QuitStudio, window, cx| {
                if this.request_navigation(PendingNavigation::Exit) {
                    window.remove_window();
                }
                cx.notify();
            }))
            .child(titlebar)
            .child(toolbar)
            .child(
                div()
                    .flex_1()
                    .h_full()
                    .min_h(px(0.))
                    .flex()
                    .child(explorer)
                    .child(
                        div()
                            .flex_1()
                            .h_full()
                            .min_h(px(0.))
                            .min_w(px(300.))
                            .flex()
                            .flex_col()
                            .when(self.workspace == "code", |center| center.child(tabs))
                            .child(viewport)
                            .when(self.workspace == "code", |center| {
                                center.child(source_editor).child(console)
                            }),
                    )
                    .child(inspector),
            )
            .child(footer);

        root.when(self.confirm_unsaved, |root| {
            root.child(
                div()
                    .absolute()
                    .inset_0()
                    .flex()
                    .items_center()
                    .justify_center()
                    .bg(rgb(0x000000))
                    .child(
                        div()
                            .w(px(390.))
                            .p_5()
                            .bg(rgb(0x292929))
                            .border_1()
                            .border_color(rgb(0x555555))
                            .shadow_lg()
                            .flex()
                            .flex_col()
                            .gap_3()
                            .child(label("Unsaved changes", 0xe5e5e5, 17.))
                            .child(label(
                                "Save your changes before leaving the editor?",
                                MUTED,
                                11.,
                            ))
                            .child(
                                div()
                                    .mt_2()
                                    .flex()
                                    .justify_end()
                                    .gap_2()
                                    .child(
                                        div()
                                            .id("unsaved-cancel")
                                            .on_click(cx.listener(|this, _, _, cx| {
                                                this.confirm_unsaved = false;
                                                this.pending_navigation = None;
                                                cx.notify();
                                            }))
                                            .child(action("Cancel", false)),
                                    )
                                    .child(
                                        div()
                                            .id("unsaved-discard")
                                            .on_click(cx.listener(|this, _, window, cx| {
                                                this.finish_pending_navigation(false, window);
                                                cx.notify();
                                            }))
                                            .child(action("Don't Save", false)),
                                    )
                                    .child(
                                        div()
                                            .id("unsaved-save")
                                            .on_click(cx.listener(|this, _, window, cx| {
                                                this.finish_pending_navigation(true, window);
                                                cx.notify();
                                            }))
                                            .child(action("Save", true)),
                                    ),
                            ),
                    ),
            )
        })
    }
}

struct Splash;

fn splash_path() -> PathBuf {
    let mut candidates = Vec::new();
    if let Ok(executable) = std::env::current_exe() {
        if let Some(directory) = executable.parent() {
            candidates.push(directory.join("splash.png"));
            candidates.push(directory.join("../Resources/splash.png"));
        }
    }
    candidates.push(PathBuf::from(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/splash.png"
    )));
    candidates
        .into_iter()
        .find(|path| path.is_file())
        .unwrap_or_else(|| PathBuf::from("splash.png"))
}

impl Render for Splash {
    fn render(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .size_full()
            .bg(rgb(0x111111))
            .flex()
            .items_center()
            .justify_center()
            .child(
                img(splash_path())
                    .size_full()
                    .object_fit(gpui::ObjectFit::Cover),
            )
    }
}

fn main() {
    Application::new().run(|cx: &mut App| {
        cx.bind_keys([
            KeyBinding::new("cmd-o", OpenProject, None),
            KeyBinding::new("cmd-s", SaveDocument, None),
            KeyBinding::new("cmd-r", RefreshProject, None),
            KeyBinding::new("cmd-q", QuitStudio, None),
        ]);
        cx.set_menus(vec![
            Menu {
                name: "Astra Studio".into(),
                items: vec![
                    MenuItem::os_submenu("Services", SystemMenuType::Services),
                    MenuItem::separator(),
                    MenuItem::action("About Astra Studio", ShowAbout),
                    MenuItem::separator(),
                    MenuItem::action("Quit Astra Studio", QuitStudio),
                ],
            },
            Menu {
                name: "File".into(),
                items: vec![
                    MenuItem::action("Open Project…", OpenProject),
                    MenuItem::separator(),
                    MenuItem::action("Save", SaveDocument),
                    MenuItem::separator(),
                    MenuItem::action("Close Window", QuitStudio),
                ],
            },
            Menu {
                name: "Edit".into(),
                items: vec![MenuItem::action("Save Document", SaveDocument)],
            },
            Menu {
                name: "View".into(),
                items: vec![
                    MenuItem::action("Home", GoHome),
                    MenuItem::action("Learn", ShowLearn),
                    MenuItem::separator(),
                    MenuItem::action("Refresh Project", RefreshProject),
                ],
            },
            Menu {
                name: "Build".into(),
                items: vec![
                    MenuItem::action("Build Active Example", BuildProject),
                    MenuItem::action("Run Active Example", RunProject),
                ],
            },
            Menu {
                name: "Help".into(),
                items: vec![MenuItem::action("About Astra Studio", ShowAbout)],
            },
        ]);
        cx.on_action(|_: &QuitStudio, cx| cx.quit());
        cx.on_window_closed(|cx| {
            if cx.windows().is_empty() {
                cx.quit();
            }
        })
        .detach();
        let splash_bounds = Bounds::centered(None, size(px(560.), px(360.)), cx);
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(splash_bounds)),
                titlebar: Some(gpui::TitlebarOptions {
                    title: Some("Astra Studio".into()),
                    appears_transparent: true,
                    traffic_light_position: Some(point(px(14.), px(14.))),
                    ..Default::default()
                }),
                ..Default::default()
            },
            |window, cx| {
                let view = cx.new(|_| Splash);
                let splash_window = window
                    .window_handle()
                    .downcast::<Splash>()
                    .expect("splash window handle");
                cx.spawn(async move |cx| {
                    Timer::after(Duration::from_millis(1250)).await;
                    let _ = cx.update(|cx| {
                        let bounds = Bounds::centered(None, size(px(1540.), px(980.)), cx);
                        cx.open_window(
                            WindowOptions {
                                window_bounds: Some(WindowBounds::Windowed(bounds)),
                                titlebar: Some(gpui::TitlebarOptions {
                                    title: Some("Astra Studio".into()),
                                    appears_transparent: true,
                                    traffic_light_position: Some(point(px(14.), px(14.))),
                                    ..Default::default()
                                }),
                                ..Default::default()
                            },
                            |window, cx| {
                                let view = cx.new(|cx| Studio::new(cx));
                                let weak_view = view.downgrade();
                                window.on_window_should_close(cx, move |_, cx| {
                                    weak_view
                                        .update(cx, |studio, cx| {
                                            if studio.dirty {
                                                studio.pending_navigation =
                                                    Some(PendingNavigation::Exit);
                                                studio.confirm_unsaved = true;
                                                cx.notify();
                                                false
                                            } else {
                                                true
                                            }
                                        })
                                        .unwrap_or(true)
                                });
                                view
                            },
                        )
                        .expect("open Astra Studio main window");
                        let _ = splash_window.update(cx, |_, window, _| window.remove_window());
                    });
                })
                .detach();
                view
            },
        )
        .expect("open Astra Studio");
        cx.activate(true);
    });
}

#[cfg(test)]
mod tests {
    use super::{LearningManifest, component_file_exists, glb_json_chunk, inspect_gltf};
    use std::{fs, path::Path, time::SystemTime};

    fn fixture_path(extension: &str) -> std::path::PathBuf {
        let nonce = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .expect("clock after epoch")
            .as_nanos();
        std::env::temp_dir().join(format!(
            "astra-studio-gltf-{}-{nonce}.{extension}",
            std::process::id()
        ))
    }

    #[test]
    fn inspects_gltf_json_asset_metadata() {
        let path = fixture_path("gltf");
        let document = r#"{"asset":{"version":"2.0","generator":"Astra test"},"scenes":[{}],"nodes":[{},{}],"meshes":[{}],"materials":[{},{}],"animations":[]}"#;
        fs::write(&path, document).expect("write glTF fixture");
        let metadata = inspect_gltf(&path).expect("valid glTF metadata");
        fs::remove_file(path).expect("remove glTF fixture");
        assert_eq!(metadata.version, "2.0");
        assert_eq!(metadata.generator, "Astra test");
        assert_eq!(metadata.scenes, 1);
        assert_eq!(metadata.nodes, 2);
        assert_eq!(metadata.meshes, 1);
        assert_eq!(metadata.materials, 2);
        assert_eq!(metadata.animations, 0);
    }

    #[test]
    fn inspects_glb_2_json_chunk_metadata() {
        let path = fixture_path("glb");
        let mut json = br#"{"asset":{"version":"2.0"},"nodes":[{}]}"#.to_vec();
        while json.len() % 4 != 0 {
            json.push(b' ');
        }
        let total_length = 12 + 8 + json.len();
        let mut glb = Vec::with_capacity(total_length);
        glb.extend_from_slice(b"glTF");
        glb.extend_from_slice(&2u32.to_le_bytes());
        glb.extend_from_slice(&(total_length as u32).to_le_bytes());
        glb.extend_from_slice(&(json.len() as u32).to_le_bytes());
        glb.extend_from_slice(&0x4e4f534au32.to_le_bytes());
        glb.extend_from_slice(&json);
        fs::write(&path, glb).expect("write GLB fixture");
        let metadata = inspect_gltf(&path).expect("valid GLB metadata");
        fs::remove_file(path).expect("remove GLB fixture");
        assert_eq!(metadata.version, "2.0");
        assert_eq!(metadata.nodes, 1);
    }

    #[test]
    fn rejects_malformed_glb_headers_and_metadata() {
        assert!(glb_json_chunk(b"not a GLB").is_err());
        let path = fixture_path("glb");
        fs::write(&path, b"glTF\x02\0").expect("write truncated GLB fixture");
        assert!(inspect_gltf(&path).is_err());
        fs::remove_file(path).expect("remove malformed GLB fixture");
    }

    #[test]
    fn learning_manifest_references_packaged_sources_and_guides() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../..")
            .canonicalize()
            .expect("repository root");
        let json = include_str!("../../../learning/manifest.json");
        let manifest: LearningManifest = serde_json::from_str(json).expect("valid manifest");
        assert_eq!(manifest.format, "astra-learning-component");
        assert_eq!(manifest.version, 1);
        assert_eq!(manifest.examples.len(), 10);
        for example in manifest.examples {
            assert!(
                component_file_exists(&root, &example.source),
                "{}",
                example.source
            );
            if let Some(guide) = example.guide {
                assert!(component_file_exists(&root, &guide), "{guide}");
            }
        }
    }
}
