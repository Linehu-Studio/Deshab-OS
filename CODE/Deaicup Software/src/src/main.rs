#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

use eframe::egui;

fn main() -> Result<(), eframe::Error>{
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([800.0, 600.0]),
        ..Default::default()
    };
    eframe::run_native(
        "Deaicup Software",
        options,
        Box::new(|cc| {
            egui_extras::install_image_loaders(&cc.egui_ctx);
            Box::new(DeaicupSoftware::default())
        },)
    )
}

#[derive(Default)]
struct DeaicupSoftware {
    current_page: Page,
}

#[derive(Debug, PartialEq, Clone, Copy, Default)]
enum Page {
    #[default]
    Home,
    Tool,
    Game,
    LinuxWare,
    WindowsWare,
    Set,
    About
}

impl eframe::App for DeaicupSoftware{
    fn update(&mut self, ctx: &egui::Context, frame: &mut eframe::Frame){
        egui::TopBottomPanel::top("top_image_panel").show(ctx, |ui|{
            let image_url = "https://deaicup.com/DeaicupSoftware/top.png";
            ui.set_min_height(75.0);
            let size = ui.available_size();
            ui.image(egui::Image::new(image_url).fit_to_exact_size(egui::Vec2::new(size.x, 75.0)));
        });
        
        //main
        egui::SidePanel::left("navigation_panel")
            .default_width(200.0)
            .resizable(true)
            .show(ctx, |ui| {
                ui.heading("Menu");
                ui.separator();
                ui.vertical(|ui| {
                    if ui.selectable_label(self.current_page == Page::Home, "HOME").clicked(){
                        self.current_page = Page::Home;
                    }
                    if ui.selectable_label(self.current_page == Page::Tool, "TOOL").clicked(){  
                        self.current_page = Page::Tool;
                    }
                    if ui.selectable_label(self.current_page == Page::Game, "GAME").clicked(){
                        self.current_page = Page::Game;
                    }
                    if ui.selectable_label(self.current_page == Page::LinuxWare, "LINUXWARE").clicked(){
                        self.current_page = Page::LinuxWare;
                    }
                    if ui.selectable_label(self.current_page == Page::WindowsWare, "WINDOWSWARE").clicked(){
                        self.current_page = Page::WindowsWare;
                    }
                    if ui.selectable_label(self.current_page == Page::Set, "SET").clicked(){
                        self.current_page = Page::Set;
                    }
                    if ui.selectable_label(self.current_page == Page::About, "ABOUT").clicked(){
                        self.current_page = Page::About;
                    }
                })
            });
        egui::CentralPanel::default().show(ctx, |ui| {
            ui.separator();
            match self.current_page {
                Page::Home => {
                    ui.label("Home")
                }
                Page::Tool => {
                    ui.label("Tool")
                }
                Page::Game => {
                    ui.label("Game")
                }
                Page::LinuxWare => {
                    ui.label("LinuxWare")
                }
                Page::WindowsWare => {
                    ui.label("WindowsWare")
                }
                Page::Set => {
                    ui.label("Set")
                }
                Page::About => {
                    ui.label("About")
                }
            }
        });
    }
}