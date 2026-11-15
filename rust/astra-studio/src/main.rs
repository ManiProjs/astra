use gpui::{
    App, Application, Bounds, Context, InteractiveElement, Window, WindowBounds, WindowControlArea,
    WindowOptions, div, point, prelude::*, px, rgb, size,
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
    output: Vec<String>,
    status: String,
    bottom_tab: String,
    scene_selection: String,
}

impl Studio {
    fn new() -> Self {
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
            source,
            output: vec![
                "Astra Studio ready".into(),
                "Project indexed · Metal renderer · C++23".into(),
            ],
            status: "Ready".into(),
            bottom_tab: "Console".into(),
            scene_selection: "Directional Light".into(),
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

    fn build(&mut self) {
        self.output
            .push("$ cmake --build build --target astra_open_world -j 4".into());
        self.status = "Building astra_open_world…".into();
        match Command::new("cmake")
            .args([
                "--build",
                "build",
                "--target",
                "astra_open_world",
                "-j",
                "4",
            ])
            .current_dir(&self.root)
            .output()
        {
            Ok(result) => {
                let stdout = String::from_utf8_lossy(&result.stdout).to_string();
                let stderr = String::from_utf8_lossy(&result.stderr).to_string();
                let lines = stdout
                    .lines()
                    .chain(stderr.lines())
                    .map(str::to_owned)
                    .collect::<Vec<_>>();
                self.output.extend(
                    lines
                        .into_iter()
                        .rev()
                        .take(70)
                        .collect::<Vec<_>>()
                        .into_iter()
                        .rev(),
                );
                self.status = if result.status.success() {
                    "Build succeeded · astra_open_world".into()
                } else {
                    format!("Build failed · exit {}", result.status.code().unwrap_or(-1))
                };
            }
            Err(error) => self.status = format!("CMake could not start: {error}"),
        }
        self.output.push(self.status.clone());
        if self.output.len() > 100 {
            self.output.drain(..self.output.len() - 100);
        }
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
                                this.build();
                                cx.notify();
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
            .child(panel_heading("Hierarchy", "＋  ···"))
            .child(
                div()
                    .px_3()
                    .py_2()
                    .child(label("▾  OPEN WORLD", 0xc4c4c4, 10.)),
            )
            .child(div().flex().flex_col().children([
                scene_row("World", 1., "▾", 0),
                scene_row("Environment", 2., "▾", 1),
                scene_row("Directional Light", 3., "☼", 2),
                scene_row("Sky & Atmosphere", 3., "◌", 3),
                scene_row("Terrain", 2., "▰", 4),
                scene_row("Player", 2., "◇", 5),
                scene_row("Camera", 3., "⌖", 6),
                scene_row("Boar", 2., "◇", 7),
            ]))
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
                            .child(label("DIRECTIONAL LIGHT", DIM, 8.)),
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
                "Position",
                [("X", "0.00"), ("Y", "12.50"), ("Z", "0.00")],
            ))
            .child(property_group(
                "Rotation",
                [("X", "-45.00°"), ("Y", "35.00°"), ("Z", "0.00°")],
            ))
            .child(property_group(
                "Scale",
                [("X", "1.00"), ("Y", "1.00"), ("Z", "1.00")],
            ))
            .child(div().h(px(1.)).mx_3().my_2().bg(rgb(BORDER)))
            .child(
                div()
                    .px_3()
                    .py_2()
                    .flex()
                    .items_center()
                    .justify_between()
                    .child(eyebrow("Light component"))
                    .child(label("⌄", MUTED, 11.)),
            )
            .child(property_group(
                "Illumination",
                [
                    ("Intensity", "3.50 lx"),
                    ("Color", "  255  255  255"),
                    ("Shadows", "Enabled"),
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
                    .child(label(selected_title, 0xbdbdbd, 10.)),
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

        let code_lines = self
            .source
            .lines()
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
                            .child(if line.is_empty() {
                                " ".into()
                            } else {
                                line.to_string()
                            }),
                    )
            })
            .collect::<Vec<_>>();
        let source_editor = div()
            .flex()
            .flex_col()
            .bg(rgb(BG))
            .border_t_1()
            .border_color(rgb(BORDER))
            .child(
                div()
                    .h(px(27.))
                    .px_3()
                    .flex()
                    .items_center()
                    .justify_between()
                    .bg(rgb(SURFACE))
                    .child(label(format!("{}  /  {}", document_name, "C++"), MUTED, 9.))
                    .child(label("READ ONLY", DIM, 8.)),
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
                    .child(label("Ln 1, Col 1", MUTED, 8.))
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

fn property_group(name: &str, values: [(&str, &str); 3]) -> gpui::Div {
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
            |_, cx| cx.new(|_| Studio::new()),
        )
        .expect("open Astra Studio");
        cx.activate(true);
    });
}
