use gpui::{
    App, Application, Bounds, Context, FocusHandle, InteractiveElement, Window, WindowBounds,
    WindowControlArea, WindowOptions, div, point, prelude::*, px, rgb, size,
};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
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

#[derive(Clone, Serialize, Deserialize)]
struct ProjectConfig {
    name: String,
    version: u32,
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
    root: PathBuf,
    project: ProjectConfig,
    files: Vec<PathBuf>,
    selected: Option<PathBuf>,
    document: Option<PathBuf>,
    source: String,
    cursor: usize,
    dirty: bool,
    editor_focus: FocusHandle,
    output: Vec<String>,
    status: String,
    bottom_tab: String,
    scene_selection: String,
    scene_entities: Vec<String>,
}

impl Studio {
    fn new(cx: &mut Context<Self>) -> Self {
        let root = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
        let project = fs::read_to_string(root.join("astra.project.json"))
            .ok()
            .and_then(|json| serde_json::from_str(&json).ok())
            .unwrap_or_default();
        let files = scan_project(&root);
        let starter = root.join("examples/open_world/main.cpp");
        let source = fs::read_to_string(&starter).unwrap_or_default();
        Self {
            root,
            project,
            files,
            selected: None,
            document: starter.is_file().then_some(starter),
            cursor: source.len(),
            source,
            dirty: false,
            editor_focus: cx.focus_handle(),
            output: vec![
                "Astra Studio ready".into(),
                "Project indexed · Metal renderer · C++23".into(),
            ],
            status: "Ready".into(),
            bottom_tab: "Console".into(),
            scene_selection: "World".into(),
            scene_entities: Vec::new(),
        }
    }

    fn open_selected(&mut self) {
        let Some(path) = self.selected.clone() else {
            self.status = "Select a project file first".into();
            return;
        };
        let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("");
        if matches!(
            ext,
            "cpp" | "hpp" | "h" | "mm" | "metal" | "rs" | "json" | "md" | "txt" | "cmake" | "py"
        ) {
            match fs::read_to_string(&path) {
                Ok(text) => {
                    self.document = Some(path.clone());
                    self.source = text;
                    self.cursor = self.source.len();
                    self.dirty = false;
                    self.load_scene_entities();
                    self.status = format!("Opened {}", path.display());
                }
                Err(error) => self.status = format!("Read failed: {error}"),
            }
        } else if ext == "blend" {
            match Command::new("open")
                .args(["-a", "Blender"])
                .arg(&path)
                .spawn()
            {
                Ok(_) => self.status = format!("Blender opened {}", path.display()),
                Err(error) => self.status = format!("Blender launch failed: {error}"),
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
                self.load_scene_entities();
                self.status = format!("Saved {}", path.display());
            }
            Err(error) => self.status = format!("Save failed: {error}"),
        }
        self.output.push(self.status.clone());
    }

    fn edit_key(&mut self, key: &gpui::KeyDownEvent) {
        let stroke = &key.keystroke;
        if stroke.modifiers.secondary() && stroke.key.eq_ignore_ascii_case("s") {
            self.save_document();
            return;
        }
        if stroke.modifiers.modified() {
            return;
        }

        match stroke.key.as_str() {
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
            "left" if self.cursor > 0 => {
                self.cursor = self.source[..self.cursor]
                    .char_indices()
                    .next_back()
                    .map(|(index, _)| index)
                    .unwrap_or(0);
            }
            "right" if self.cursor < self.source.len() => {
                self.cursor += self.source[self.cursor..]
                    .chars()
                    .next()
                    .map(char::len_utf8)
                    .unwrap_or(0);
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
        self.source.insert_str(self.cursor, text);
        self.cursor += text.len();
        self.dirty = true;
    }

    fn refresh_files(&mut self) {
        self.files = scan_project(&self.root);
        self.status = format!("Project refreshed · {} files indexed", self.files.len());
        self.output.push(self.status.clone());
    }

    fn load_scene_entities(&mut self) {
        self.scene_entities = scene_entity_names(&self.source);
    }

    fn add_entity(&mut self) {
        let Some(path) = self.document.as_ref() else {
            self.status = "Create or open a scene document first".into();
            return;
        };
        if path.extension().and_then(|ext| ext.to_str()) != Some("json") {
            self.status = "Entities can only be added to an Astra scene JSON document".into();
            return;
        }
        let (source, name) = match append_scene_entity(&self.source) {
            Ok(result) => result,
            Err(error) => {
                self.status = error;
                return;
            }
        };
        self.source = source;
        self.cursor = self.source.len();
        self.dirty = true;
        self.scene_entities.push(name.clone());
        self.scene_selection = name;
        self.status = "Entity added · ⌘S to save scene".into();
        self.output.push(self.status.clone());
    }

    fn create_scene(&mut self) {
        let directory = self.root.join("Scenes");
        let mut index = 1;
        let path = loop {
            let filename = if index == 1 {
                "NewScene.json".to_owned()
            } else {
                format!("NewScene{index}.json")
            };
            let candidate = directory.join(filename);
            if !candidate.exists() {
                break candidate;
            }
            index += 1;
        };
        let scene = serde_json::json!({
            "format": "astra-scene",
            "version": 1,
            "name": path.file_stem().unwrap_or_default().to_string_lossy(),
            "entities": []
        });
        let result = fs::create_dir_all(&directory).and_then(|()| {
            fs::write(
                &path,
                format!("{}\n", serde_json::to_string_pretty(&scene).unwrap()),
            )
        });
        match result {
            Ok(()) => {
                self.refresh_files();
                self.selected = Some(path.clone());
                self.document = Some(path.clone());
                self.source = fs::read_to_string(&path).unwrap_or_default();
                self.cursor = self.source.len();
                self.dirty = false;
                self.load_scene_entities();
                self.scene_selection = "World".into();
                self.status = format!("Created {}", path.display());
            }
            Err(error) => self.status = format!("Scene creation failed: {error}"),
        }
        self.output.push(self.status.clone());
    }

    fn build(&mut self, cx: &mut Context<Self>) {
        let root = self.root.clone();
        self.output
            .push("$ cmake --build build --target astra_open_world -j 4".into());
        self.status = "Building astra_open_world…".into();
        let executor = cx.background_executor().clone();
        cx.notify();
        cx.spawn(async move |this, cx| {
            let result = executor
                .spawn(async move {
                    Command::new("cmake")
                        .args([
                            "--build",
                            "build",
                            "--target",
                            "astra_open_world",
                            "-j",
                            "4",
                        ])
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
                            "Build succeeded · astra_open_world".into()
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
        let candidates = [
            self.root.join("build/Debug/astra_open_world"),
            self.root.join("build/astra_open_world"),
            self.root.join("cmake-build-debug/astra_open_world"),
        ];
        if let Some(binary) = candidates.iter().find(|p| p.is_file()) {
            self.status = match Command::new(binary).current_dir(&self.root).spawn() {
                Ok(_) => "Runtime launched in its native window".into(),
                Err(e) => format!("Runtime launch failed: {e}"),
            };
        } else {
            self.status = "Runtime not built. Use Build first.".into();
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
                    "cpp"
                        | "hpp"
                        | "h"
                        | "mm"
                        | "metal"
                        | "rs"
                        | "blend"
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

fn scene_entity_names(source: &str) -> Vec<String> {
    let Ok(value) = serde_json::from_str::<serde_json::Value>(source) else {
        return Vec::new();
    };
    if value.get("format").and_then(|v| v.as_str()) != Some("astra-scene") {
        return Vec::new();
    }
    value
        .get("entities")
        .and_then(|v| v.as_array())
        .into_iter()
        .flatten()
        .filter_map(|entity| entity.get("name").and_then(|name| name.as_str()))
        .map(str::to_owned)
        .collect()
}

fn append_scene_entity(source: &str) -> Result<(String, String), String> {
    let mut scene = serde_json::from_str::<serde_json::Value>(source)
        .map_err(|_| "The open JSON document is not a valid scene".to_owned())?;
    if scene.get("format").and_then(|v| v.as_str()) != Some("astra-scene") {
        return Err("The open JSON document is not an Astra scene".into());
    }
    let entities = scene["entities"]
        .as_array_mut()
        .ok_or_else(|| "Scene entities must be a JSON array".to_owned())?;
    let name = format!("Entity {}", entities.len() + 1);
    entities.push(serde_json::json!({
        "name": name,
        "transform": {
            "position": [0.0, 0.0, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0]
        }
    }));
    let serialized = serde_json::to_string_pretty(&scene)
        .map_err(|error| format!("Scene serialization failed: {error}"))?;
    Ok((serialized + "\n", name))
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
fn tree_row(name: &str, depth: f32, glyph: &str, selected: bool) -> gpui::Div {
    div()
        .h(px(24.))
        .pl(px(10. + depth * 13.))
        .pr_2()
        .flex()
        .items_center()
        .gap_2()
        .bg(rgb(if selected { SELECTED } else { PANEL }))
        .child(label(glyph, MUTED, 11.))
        .child(label(name, if selected { TEXT } else { 0xbcbcbc }, 11.))
}

impl Render for Studio {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
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
                        div()
                            .flex()
                            .items_center()
                            .gap_1()
                            .child(action("Scene", true))
                            .child(action("Script", false))
                            .child(action("Assets", false)),
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
                            .id("refresh-project")
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.refresh_files();
                                cx.notify();
                            }))
                            .child(action("Refresh", false)),
                    )
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap_1()
                            .child(label("◉", MUTED, 11.))
                            .child(label("Debug", 0xbdbdbd, 10.))
                            .child(label("⌄", MUTED, 10.)),
                    )
                    .child(div().w(px(1.)).h(px(22.)).bg(rgb(BORDER)))
                    .child(
                        div()
                            .id("build")
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.build(cx);
                            }))
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
                    .child(label("Ⅱ", MUTED, 12.))
                    .child(label("■", MUTED, 11.))
                    .child(div().w(px(1.)).h(px(22.)).bg(rgb(BORDER)))
                    .child(label("···", MUTED, 15.)),
            );

        let files = self.files.iter().take(38).cloned().collect::<Vec<_>>();
        let assets = self
            .files
            .iter()
            .filter(|path| {
                matches!(
                    path.extension().and_then(|ext| ext.to_str()),
                    Some("blend" | "glb" | "gltf" | "hdr" | "png" | "jpg" | "jpeg")
                )
            })
            .take(8)
            .cloned()
            .collect::<Vec<_>>();
        let scene_row = |name: &str, depth: f32, glyph: &str, index: usize| {
            let target = name.to_string();
            div()
                .id(("scene-row", index))
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.scene_selection = target.clone();
                    cx.notify();
                }))
                .child(tree_row(name, depth, glyph, self.scene_selection == name))
        };
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
                    .child(eyebrow("Hierarchy"))
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap_1()
                            .child(
                                div()
                                    .id("new-scene")
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.create_scene();
                                        cx.notify();
                                    }))
                                    .child(action("＋ Scene", false)),
                            )
                            .child(
                                div()
                                    .id("new-entity")
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.add_entity();
                                        cx.notify();
                                    }))
                                    .child(action("＋ Entity", false)),
                            ),
                    ),
            )
            .child(
                div()
                    .px_3()
                    .py_2()
                    .child(label("▾  OPEN WORLD", 0xc4c4c4, 10.)),
            )
            .child(
                div()
                    .flex()
                    .flex_col()
                    .children([scene_row("World", 1., "▾", 0)]),
            )
            .children(
                self.scene_entities
                    .iter()
                    .enumerate()
                    .map(|(index, name)| scene_row(name, 2., "◇", index + 1)),
            )
            .child(div().h(px(1.)).mx_3().my_2().bg(rgb(BORDER)))
            .child(
                div()
                    .px_3()
                    .pb_2()
                    .flex()
                    .items_center()
                    .justify_between()
                    .child(eyebrow("Project files"))
                    .child(label(format!("{} ITEMS", self.files.len()), DIM, 8.)),
            )
            .children(files.into_iter().enumerate().map(|(i, path)| {
                let rel = path
                    .strip_prefix(&self.root)
                    .unwrap_or(&path)
                    .display()
                    .to_string();
                let name = path
                    .file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or(rel.clone());
                let chosen = selected_path.as_ref() == Some(&path);
                let target = path.clone();
                div()
                    .id(("file", i))
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.selected = Some(target.clone());
                        this.status = format!("Selected {}", target.display());
                        cx.notify();
                    }))
                    .h(px(22.))
                    .px_3()
                    .flex()
                    .items_center()
                    .gap_2()
                    .bg(rgb(if chosen { SELECTED } else { PANEL }))
                    .child(label(
                        if path.extension().and_then(|e| e.to_str()) == Some("glb") {
                            "◇"
                        } else {
                            "·"
                        },
                        MUTED,
                        12.,
                    ))
                    .child(label(name, if chosen { TEXT } else { 0xa9a9a9 }, 10.))
            }))
            .child(div().h(px(1.)).mx_3().my_2().bg(rgb(BORDER)))
            .child(
                div()
                    .px_3()
                    .pb_2()
                    .flex()
                    .items_center()
                    .justify_between()
                    .child(eyebrow("Asset browser"))
                    .child(label("GRID  ≡", DIM, 8.)),
            )
            .children(assets.into_iter().enumerate().map(|(index, path)| {
                let name = path
                    .file_name()
                    .map(|name| name.to_string_lossy().to_string())
                    .unwrap_or_default();
                let target = path.clone();
                div()
                    .id(("asset", index))
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.selected = Some(target.clone());
                        this.status = format!("Selected asset {}", target.display());
                        cx.notify();
                    }))
                    .h(px(22.))
                    .px_3()
                    .flex()
                    .items_center()
                    .gap_2()
                    .child(label("◇", MUTED, 11.))
                    .child(label(name, 0xb5b5b5, 10.))
            }))
            .child(div().flex_1())
            .child(
                div()
                    .border_t_1()
                    .border_color(rgb(BORDER))
                    .px_3()
                    .py_2()
                    .child(label("⌘ P   QUICK OPEN", DIM, 9.)),
            );

        let selected_title = selected_path
            .as_ref()
            .map(|p| {
                p.file_name()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .to_string()
            })
            .unwrap_or_else(|| "No selection".into());
        let entity_count = self.scene_entities.len().to_string();
        let inspector = div()
            .w(px(252.))
            .min_w(px(205.))
            .flex()
            .flex_col()
            .bg(rgb(PANEL))
            .border_l_1()
            .border_color(rgb(BORDER))
            .child(panel_heading("Inspector", "···"))
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
                            .child(label(self.scene_selection.clone(), TEXT, 11.))
                            .child(label("SCENE SELECTION", DIM, 8.)),
                    )
                    .child(label("◉", MUTED, 11.)),
            )
            .child(
                div()
                    .px_3()
                    .py_2()
                    .flex()
                    .items_center()
                    .justify_between()
                    .child(eyebrow("Transform"))
                    .child(label("⌄", MUTED, 11.)),
            )
            .child(property_group(
                "Scene",
                [
                    ("Entities", entity_count),
                    ("Format", "Astra JSON".into()),
                    ("Version", "1".into()),
                ],
            ))
            .child(div().h(px(1.)).mx_3().my_2().bg(rgb(BORDER)))
            .child(
                div()
                    .px_3()
                    .py_2()
                    .flex()
                    .items_center()
                    .justify_between()
                    .child(eyebrow("Selection details"))
                    .child(label("⌄", MUTED, 11.)),
            )
            .child(
                div()
                    .px_3()
                    .pb_3()
                    .child(label(selected_title, 0xbdbdbd, 10.))
                    .child(label(
                        selected_path
                            .as_ref()
                            .and_then(|path| fs::metadata(path).ok())
                            .map(|metadata| format!("{} bytes", metadata.len()))
                            .unwrap_or_else(|| "No file selected".into()),
                        DIM,
                        9.,
                    )),
            )
            .child(div().flex_1())
            .child(
                div()
                    .border_t_1()
                    .border_color(rgb(BORDER))
                    .px_3()
                    .py_2()
                    .child(label("COMPONENTS   ＋", MUTED, 9.)),
            );

        let viewport = div()
            .flex_1()
            .min_w(px(280.))
            .flex()
            .flex_col()
            .bg(rgb(0x171717))
            .child(
                div()
                    .h(px(34.))
                    .px_3()
                    .flex()
                    .items_center()
                    .justify_between()
                    .bg(rgb(SURFACE))
                    .border_b_1()
                    .border_color(rgb(BORDER))
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap_3()
                            .child(label("Perspective", 0xc8c8c8, 10.))
                            .child(label("⌄", MUTED, 10.))
                            .child(label("│", DIM, 10.))
                            .child(label("Shaded", MUTED, 10.)),
                    )
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap_3()
                            .child(label("Gizmo  ⌄", MUTED, 9.))
                            .child(label("Grid  ◉", MUTED, 9.))
                            .child(label("⛶", MUTED, 12.)),
                    ),
            )
            .child(
                div()
                    .flex_1()
                    .relative()
                    .overflow_hidden()
                    .flex()
                    .items_center()
                    .justify_center()
                    .child(
                        div()
                            .absolute()
                            .inset_0()
                            .flex()
                            .flex_col()
                            .justify_between()
                            .children((0..14).map(|_| div().h(px(1.)).w_full().bg(rgb(0x202020)))),
                    )
                    .child(
                        div()
                            .absolute()
                            .inset_0()
                            .flex()
                            .justify_between()
                            .children((0..22).map(|_| div().w(px(1.)).h_full().bg(rgb(0x202020)))),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .items_center()
                            .gap_3()
                            .child(label("◫", 0x555555, 44.))
                            .child(label("SCENE VIEWPORT", 0x777777, 10.))
                            .child(label("Metal renderer surface will appear here", DIM, 9.)),
                    )
                    .child(
                        div()
                            .absolute()
                            .left(px(14.))
                            .top(px(12.))
                            .flex()
                            .flex_col()
                            .gap_1()
                            .child(label("OPEN WORLD", 0x999999, 9.))
                            .child(label("EDITOR CAMERA", DIM, 8.)),
                    )
                    .child(
                        div()
                            .absolute()
                            .right(px(14.))
                            .top(px(12.))
                            .flex()
                            .flex_col()
                            .items_end()
                            .gap_1()
                            .child(label("XYZ", 0xaaaaaa, 10.))
                            .child(label("PERSPECTIVE", DIM, 8.)),
                    )
                    .child(
                        div()
                            .absolute()
                            .left(px(14.))
                            .bottom(px(12.))
                            .flex()
                            .items_center()
                            .gap_3()
                            .child(label("↔  MOVE", MUTED, 9.))
                            .child(label("⟳  ROTATE", MUTED, 9.))
                            .child(label("⇲  SCALE", MUTED, 9.)),
                    ),
            )
            .child(
                div()
                    .h(px(26.))
                    .px_3()
                    .flex()
                    .items_center()
                    .justify_between()
                    .bg(rgb(SURFACE))
                    .border_t_1()
                    .border_color(rgb(BORDER))
                    .child(label("WORLD  /  OPEN WORLD  /  EDITOR CAMERA", DIM, 8.))
                    .child(label("100%   ·   24 FPS", MUTED, 8.)),
            );

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
                    .child(label("×", MUTED, 12.)),
            )
            .child(div().px_3().child(label("+", MUTED, 14.)))
            .child(div().flex_1())
            .child(div().px_3().child(label("SOURCE", DIM, 8.)));

        let cursor_line = self.source[..self.cursor].matches('\n').count();
        let line_start = self.source[..self.cursor]
            .rfind('\n')
            .map(|index| index + 1)
            .unwrap_or(0);
        let cursor_column = self.source[line_start..self.cursor].chars().count();
        let code_lines = self
            .source
            .split('\n')
            .take(180)
            .enumerate()
            .map(|(i, line)| {
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
                this.edit_key(event);
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
                    .h(px(150.))
                    .overflow_hidden()
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
                    .child(
                        div().flex().items_center().gap_4().children(
                            ["Console", "Build", "Diagnostics", "Asset Imports"]
                                .into_iter()
                                .enumerate()
                                .map(|(index, tab)| {
                                    let active = self.bottom_tab == tab;
                                    div()
                                        .id(("bottom-tab", index))
                                        .on_click(cx.listener(move |this, _, _, cx| {
                                            this.bottom_tab = tab.into();
                                            cx.notify();
                                        }))
                                        .h_full()
                                        .flex()
                                        .items_center()
                                        .border_b_1()
                                        .border_color(rgb(if active { 0xb0b0b0 } else { SURFACE }))
                                        .child(label(tab, if active { TEXT } else { MUTED }, 9.))
                                }),
                        ),
                    )
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
                    .child(label("METAL", MUTED, 8.))
                    .child(label("C++23", MUTED, 8.))
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

        div()
            .size_full()
            .flex()
            .flex_col()
            .bg(rgb(BG))
            .text_color(rgb(TEXT))
            .child(titlebar)
            .child(toolbar)
            .child(
                div()
                    .flex_1()
                    .min_h(px(300.))
                    .flex()
                    .child(explorer)
                    .child(
                        div()
                            .flex_1()
                            .min_w(px(300.))
                            .flex()
                            .flex_col()
                            .child(tabs)
                            .child(viewport)
                            .child(source_editor)
                            .child(console),
                    )
                    .child(inspector),
            )
            .child(footer)
    }
}

fn property_group(name: &str, values: [(&str, String); 3]) -> gpui::Div {
    div()
        .px_3()
        .pb_2()
        .flex()
        .flex_col()
        .gap_1()
        .child(div().pb_1().child(label(name, MUTED, 9.)))
        .children(values.into_iter().map(|(key, value)| {
            div()
                .flex()
                .items_center()
                .gap_2()
                .child(div().w(px(52.)).child(label(key, 0x999999, 9.)))
                .child(
                    div()
                        .flex_1()
                        .h(px(22.))
                        .px_2()
                        .flex()
                        .items_center()
                        .bg(rgb(0x1d1d1d))
                        .border_1()
                        .border_color(rgb(BORDER))
                        .child(label(value, 0xc4c4c4, 9.)),
                )
        }))
}

fn main() {
    Application::new().run(|cx: &mut App| {
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
            |_, cx| cx.new(|cx| Studio::new(cx)),
        )
        .expect("open Astra Studio");
        cx.activate(true);
    });
}

#[cfg(test)]
mod tests {
    use super::{append_scene_entity, scene_entity_names};

    #[test]
    fn reads_entity_names_from_astra_scene_documents() {
        let scene = r#"{"format":"astra-scene","entities":[{"name":"Camera"},{"name":"Terrain"}]}"#;
        assert_eq!(scene_entity_names(scene), ["Camera", "Terrain"]);
    }

    #[test]
    fn appends_entity_with_default_transform() {
        let scene = r#"{"format":"astra-scene","version":1,"entities":[{"name":"Camera"}]}"#;
        let (updated, name) = append_scene_entity(scene).expect("valid scene");
        assert_eq!(name, "Entity 2");
        let value: serde_json::Value = serde_json::from_str(&updated).expect("valid JSON");
        assert_eq!(value["entities"][1]["name"], "Entity 2");
        assert_eq!(
            value["entities"][1]["transform"]["scale"],
            serde_json::json!([1.0, 1.0, 1.0])
        );
    }

    #[test]
    fn rejects_non_astra_json_documents() {
        assert!(append_scene_entity(r#"{"name":"settings"}"#).is_err());
        assert!(append_scene_entity("not JSON").is_err());
    }
}
