use crate::updater;
use crate::{headers, probe_scale, Editor, SECTIONS};
use eframe::egui;
use std::path::PathBuf;

pub fn launch(path: Option<PathBuf>) -> Result<(), eframe::Error> {
    let native = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([1200.0, 760.0]),
        ..Default::default()
    };
    eframe::run_native(
        "TMS / DLX-50-style Editor",
        native,
        Box::new(move |_| Ok(Box::new(TmsApp::new(path)))),
    )
}
struct TmsApp {
    editor: Option<Editor>,
    path: Option<PathBuf>,
    selected: usize,
    status: String,
    probe_host: String,
    probe_port: String,
    probe_timeout: String,
    probe_output: String,
    update_output: String,
    selected_rows: Vec<usize>,
    skip_updates: bool,
    pending_release: Option<updater::Release>,
    startup_rx: Option<std::sync::mpsc::Receiver<Result<Option<updater::Release>, String>>>,
    install_rx: Option<std::sync::mpsc::Receiver<Result<String, String>>>,
}
impl TmsApp {
    fn new(path: Option<PathBuf>) -> Self {
        let (editor, status) = match path.as_ref() {
            Some(p) => match Editor::open(p) {
                Ok(e) => (Some(e), format!("Loaded: {}", p.display())),
                Err(e) => (None, format!("Could not load {}: {e}", p.display())),
            },
            None => (None, "Open a .TMS file to begin".into()),
        };
        let args: Vec<String> = std::env::args().collect();
        let skip_updates = args
            .iter()
            .any(|a| a.eq_ignore_ascii_case("--no-update-check"))
            || !updater::startup_check_allowed();
        let startup_rx = if skip_updates {
            None
        } else {
            Some(updater::schedule_startup_check())
        };
        Self {
            editor,
            path,
            selected: 0,
            status,
            probe_host: "10.86.215.95".into(),
            probe_port: "9100".into(),
            probe_timeout: "10000".into(),
            probe_output: String::new(),
            update_output: String::new(),
            selected_rows: Vec::new(),
            skip_updates,
            pending_release: None,
            startup_rx,
            install_rx: None,
        }
    }
    fn open(&mut self) {
        if let Some(p) = rfd::FileDialog::new()
            .add_filter("TMS files", &["TMS", "tms"])
            .pick_file()
        {
            match Editor::open(&p) {
                Ok(e) => {
                    self.path = Some(p.clone());
                    self.editor = Some(e);
                    self.status = format!("Loaded: {}", p.display());
                }
                Err(e) => self.status = format!("Cannot open: {e}"),
            }
        }
    }
    fn save_to(&mut self, path: PathBuf, backup: bool) {
        if let Some(e) = &self.editor {
            let result = if backup {
                e.save(&path)
            } else {
                std::fs::write(&path, e.serialize()).map_err(crate::EditError::Io)
            };
            match result {
                Ok(()) => {
                    self.path = Some(path.clone());
                    self.status = format!("Saved: {}", path.display());
                }
                Err(err) => self.status = format!("Save failed: {err}"),
            }
        }
    }
    fn save(&mut self) {
        if let Some(p) = self.path.clone() {
            self.save_to(p, true)
        } else {
            self.save_as()
        }
    }
    fn save_as(&mut self) {
        if let Some(p) = rfd::FileDialog::new()
            .add_filter("TMS files", &["TMS", "tms"])
            .save_file()
        {
            self.save_to(p, true)
        }
    }
    fn add_row(&mut self) {
        let tag = SECTIONS[self.selected];
        if let Some(e) = &mut self.editor {
            match e.add(tag, "", "", "", "") {
                Ok(()) => self.status = format!("Added {tag} row — edit it, then Save"),
                Err(err) => self.status = err.to_string(),
            }
        }
    }
    fn delete_selected(&mut self) {
        let tag = SECTIONS[self.selected];
        if let Some(e) = &mut self.editor {
            let mut rows = self.selected_rows.clone();
            rows.sort_unstable();
            rows.dedup();
            for r in rows.into_iter().rev() {
                let _ = e.delete_row(tag, r);
            }
            self.selected_rows.clear();
            self.status = format!("Deleted selected rows from {tag} — Save to write changes")
        }
    }
    fn probe(&mut self) {
        let host = self.probe_host.trim().to_owned();
        let port = self.probe_port.parse::<u16>();
        let timeout = self.probe_timeout.parse::<u64>();
        match (port, timeout) {
            (Ok(p), Ok(t)) if !host.is_empty() && t >= 500 => {
                let r = probe_scale(&host, p, t);
                self.probe_output = format!(
                    "Target: {}:{}\nConnected: {}\nBytes: {}\nHex: {}\n\n{}",
                    r.host,
                    r.port,
                    r.connected,
                    r.raw.len(),
                    r.raw.iter().map(|b| format!("{b:02x}")).collect::<String>(),
                    r.summary
                );
                self.status = "Read-only probe complete".into();
            }
            _ => self.status = "Enter a host, port 1..65535, and timeout >= 500 ms".into(),
        }
    }
    fn check_updates(&mut self) {
        self.update_output = "Checking GitHub Releases…".into();
        match updater::fetch_latest() {
            Ok(None) => {
                self.update_output = format!(
                    "You are running the latest version (v{}).",
                    env!("TMS_APP_VERSION")
                )
            }
            Ok(Some(release)) => {
                self.update_output = format!(
                    "A new version is available: {}\nCurrently running: v{}\n\n{}",
                    release.tag,
                    env!("TMS_APP_VERSION"),
                    release.notes
                );
                self.pending_release = Some(release);
            }
            Err(e) => self.update_output = format!("Update check failed: {e}"),
        }
    }
}
impl eframe::App for TmsApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        if let Some(rx) = &self.install_rx {
            if let Some(result) = updater::poll_install(rx) {
                self.install_rx = None;
                self.update_output =
                    result.unwrap_or_else(|error| format!("Update failed: {error}"));
            } else {
                ctx.request_repaint_after(std::time::Duration::from_millis(250));
            }
        }
        if let Some(rx) = &self.startup_rx {
            if let Some(result) = updater::poll_check(rx) {
                self.startup_rx = None;
                match result {
                    Ok(None) => {}
                    Ok(Some(release)) => {
                        self.update_output = format!(
                            "A new version is available: {}\\nCurrently running: v{}",
                            release.tag,
                            env!("TMS_APP_VERSION")
                        );
                        self.pending_release = Some(release);
                    }
                    Err(e) => {
                        self.update_output = format!("Update check failed: {e}");
                    }
                }
            } else {
                ctx.request_repaint_after(std::time::Duration::from_millis(250));
            }
        }
        if ctx.input(|i| i.key_pressed(egui::Key::O) && i.modifiers.command) {
            self.open();
        }
        if ctx.input(|i| i.key_pressed(egui::Key::S) && i.modifiers.command) {
            if ctx.input(|i| i.modifiers.shift) {
                self.save_as()
            } else {
                self.save()
            }
        }
        egui::TopBottomPanel::top("menu").show(ctx, |ui| {
            egui::menu::bar(ui, |ui| {
                ui.menu_button("File", |ui| {
                    if ui.button("Open…  Ctrl+O").clicked() {
                        self.open();
                        ui.close_menu()
                    }
                    if ui.button("Save  Ctrl+S").clicked() {
                        self.save();
                        ui.close_menu()
                    }
                    if ui.button("Save As…  Ctrl+Shift+S").clicked() {
                        self.save_as();
                        ui.close_menu()
                    }
                    ui.separator();
                    ui.menu_button("Scale…", |ui| {
                        if ui
                            .button("Retrieve from scale… (not implemented)")
                            .clicked()
                        {
                            self.status = "Scale retrieval is not implemented yet.".into();
                            ui.close_menu();
                        }
                        if ui.button("Upload to scale… (not implemented)").clicked() {
                            self.status = "Scale upload is not implemented yet.".into();
                            ui.close_menu();
                        }
                        ui.separator();
                        if ui.button("Probe scale (read-only, issue #8)…").clicked() {
                            self.probe_output.clear();
                            ui.close_menu();
                        }
                    });
                });
                ui.menu_button("Help", |ui| {
                    if ui.button("Check for Updates…").clicked() {
                        self.check_updates();
                        ui.close_menu()
                    }
                    if ui
                        .checkbox(&mut self.skip_updates, "Skip startup update check")
                        .changed()
                        && self.skip_updates
                    {
                        let _ = updater::disable_startup_check();
                    }
                    ui.label(format!(
                        "TMS / DLX-50-style Editor v{}",
                        env!("TMS_APP_VERSION")
                    ));
                });
            })
        });
        egui::TopBottomPanel::bottom("status").show(ctx, |ui| ui.label(&self.status));
        egui::CentralPanel::default().show(ctx,|ui|{
            if self.editor.is_some(){
                ui.horizontal(|ui|{for (i,tag) in SECTIONS.iter().enumerate(){if ui.selectable_label(self.selected==i,*tag).clicked(){self.selected=i;self.selected_rows.clear();}}});ui.separator();
                let mut add=false;let mut delete=false;let mut save=false;let mut save_as=false;
                ui.horizontal(|ui|{add=ui.button("Add row").clicked();delete=ui.button("Delete selected").clicked();save=ui.button("Save").clicked();save_as=ui.button("Save As…").clicked()});
                if add { self.add_row(); }
                if delete { self.delete_selected(); }
                if save { self.save(); }
                if save_as { self.save_as(); }
                let tag=SECTIONS[self.selected];let rows=self.editor.as_ref().unwrap().list(tag).unwrap_or_else(|_|vec![headers(tag).iter().map(|s|s.to_string()).collect()]);
                let mut edit_error=None;
                egui::ScrollArea::both().auto_shrink([false;2]).show(ui,|ui|{egui::Grid::new(format!("table-{tag}")).striped(true).show(ui,|ui|{for h in headers(tag){ui.strong(*h);}ui.end_row();for (r,cells) in rows.iter().enumerate().skip(1){let mut checked=self.selected_rows.contains(&(r-1));if ui.checkbox(&mut checked,format!("{}",r-1)).changed(){if checked{self.selected_rows.push(r-1)}else{self.selected_rows.retain(|x|*x!=r-1)}}for (c,cell) in cells.iter().enumerate(){let mut value=cell.clone();let response=ui.add_sized([if c==4{280.0}else{150.0},22.0],egui::TextEdit::singleline(&mut value));if response.changed(){if let Some(editor)=&mut self.editor{if let Err(err)=editor.set(tag,r-1,c,&value){edit_error=Some(err.to_string());}}}}ui.end_row();}})});
                if let Some(err)=edit_error{self.status=err;}
            }else{ui.heading("TMS / DLX-50-style Editor");ui.label("Open a .TMS file to browse or edit its sections.");if ui.button("Open .TMS file…").clicked(){self.open();}}
            ui.separator();            ui.collapsing("Scale probe (read-only)", |ui| {
                ui.horizontal(|ui| {ui.label("Host");ui.text_edit_singleline(&mut self.probe_host);ui.label("Port");ui.add(egui::TextEdit::singleline(&mut self.probe_port).desired_width(70.0));ui.label("Timeout ms");ui.add(egui::TextEdit::singleline(&mut self.probe_timeout).desired_width(90.0));if ui.button("Run probe").clicked(){self.probe();}});ui.label("Sends the current 8-byte probe only; no write, delete, reboot, or transfer command.");if !self.probe_output.is_empty(){ui.add(egui::TextEdit::multiline(&mut self.probe_output).desired_rows(8).code_editor());}});
            if !self.update_output.is_empty() {
                ui.separator();
                ui.collapsing("Update check", |ui| {
                    ui.label(&self.update_output);
                    let install = self.pending_release.is_some() && ui.button("Download, verify, and install").clicked();
                    if install {
                        if let Some(release) = self.pending_release.take() {
                            self.update_output = "Downloading and verifying release…".into();
                            self.install_rx = Some(updater::schedule_install(release));
                        }
                    }
                });
            }
        });
    }
}
