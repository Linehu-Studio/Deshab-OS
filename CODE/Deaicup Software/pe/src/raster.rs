//! Pure-Rust software rasterizer for egui tessellated primitives.
//! Renders `ClippedPrimitive` meshes (font atlas + color triangles) into a
//! 32bpp 0xAARRGGBB backbuffer that is later blitted via GDI BitBlt.

use egui::epaint::Primitive;
use egui::{ClippedPrimitive, Color32, Rect, TextureFilter, TextureId, TexturesDelta};

struct Texture {
    w: usize,
    h: usize,
    /// RGBA8, row-major.
    rgba: Vec<u8>,
    linear: bool,
}

pub struct SoftRaster {
    /// Small texture store (font atlas + user images). Vec-based to avoid
    /// std HashMap RandomState (which would import an OS RNG API).
    textures: Vec<(TextureId, Texture)>,
}

impl SoftRaster {
    pub fn new() -> Self {
        SoftRaster {
            textures: Vec::new(),
        }
    }

    fn get(&self, id: TextureId) -> Option<&Texture> {
        self.textures.iter().find(|(tid, _)| *tid == id).map(|(_, t)| t)
    }

    /// Apply egui's texture updates (font atlas etc).
    pub fn apply_delta(&mut self, delta: &TexturesDelta) {
        for (id, img_delta) in &delta.set {
            let size = img_delta.image.size();
            let mut rgba = Vec::with_capacity(size[0] * size[1] * 4);
            match &img_delta.image {
                egui::ImageData::Color(color) => {
                    for p in &color.pixels {
                        rgba.extend_from_slice(&p.to_array());
                    }
                }
                egui::ImageData::Font(font) => {
                    // Coverage -> straight-alpha white texel (255,255,255,a).
                    for c in font.srgba_pixels(None) {
                        let a = c.a();
                        rgba.extend_from_slice(&[255, 255, 255, a]);
                    }
                }
            }
            let linear = img_delta.options.magnification == TextureFilter::Linear;
            let tex = Texture {
                w: size[0],
                h: size[1],
                rgba,
                linear,
            };
            if let Some(slot) = self.textures.iter_mut().find(|(tid, _)| tid == id) {
                slot.1 = tex;
            } else {
                self.textures.push((*id, tex));
            }
        }
        for id in &delta.free {
            self.textures.retain(|(tid, _)| tid != id);
        }
    }

    /// Paint all clipped primitives into `dst` (0xAARRGGBB pixels, dw*dh).
    pub fn paint(&self, dst: &mut [u32], dw: i32, dh: i32, prims: &[ClippedPrimitive]) {
        for cp in prims {
            if let Primitive::Mesh(mesh) = &cp.primitive {
                let Some(tex) = self.get(mesh.texture_id) else {
                    continue;
                };
                let clip = clip_rect_px(&cp.clip_rect, dw, dh);
                if clip.2 <= clip.0 || clip.3 <= clip.1 {
                    continue;
                }
                for tri in mesh.indices.chunks_exact(3) {
                    let v0 = &mesh.vertices[tri[0] as usize];
                    let v1 = &mesh.vertices[tri[1] as usize];
                    let v2 = &mesh.vertices[tri[2] as usize];
                    raster_tri(dst, dw, dh, clip, v0, v1, v2, tex);
                }
            }
        }
    }
}

/// (x0, y0, x1, y1) integer clip bounds from an egui rect (points == pixels here).
fn clip_rect_px(r: &Rect, dw: i32, dh: i32) -> (i32, i32, i32, i32) {
    let x0 = (r.min.x.floor() as i32).clamp(0, dw);
    let y0 = (r.min.y.floor() as i32).clamp(0, dh);
    let x1 = (r.max.x.ceil() as i32).clamp(0, dw);
    let y1 = (r.max.y.ceil() as i32).clamp(0, dh);
    (x0, y0, x1, y1)
}

#[inline]
fn edge(ax: f32, ay: f32, bx: f32, by: f32, px: f32, py: f32) -> f32 {
    (px - ax) * (by - ay) - (py - ay) * (bx - ax)
}

fn raster_tri(
    dst: &mut [u32],
    dw: i32,
    _dh: i32,
    clip: (i32, i32, i32, i32),
    v0: &egui::epaint::Vertex,
    v1: &egui::epaint::Vertex,
    v2: &egui::epaint::Vertex,
    tex: &Texture,
) {
    let x0 = v0.pos.x;
    let y0 = v0.pos.y;
    let x1 = v1.pos.x;
    let y1 = v1.pos.y;
    let x2 = v2.pos.x;
    let y2 = v2.pos.y;

    let area = edge(x0, y0, x1, y1, x2, y2);
    if area.abs() < 1e-6 {
        return;
    }
    let inv_area = 1.0 / area;

    let min_x = ((x0.min(x1).min(x2)).floor() as i32).max(clip.0);
    let min_y = ((y0.min(y1).min(y2)).floor() as i32).max(clip.1);
    let max_x = ((x0.max(x1).max(x2)).ceil() as i32).min(clip.2);
    let max_y = ((y0.max(y1).max(y2)).ceil() as i32).min(clip.3);
    if min_x >= max_x || min_y >= max_y {
        return;
    }

    for py in min_y..max_y {
        let fy = py as f32 + 0.5;
        let mut px = min_x;
        // Row start barycentrics
        let mut w0 = edge(x1, y1, x2, y2, px as f32 + 0.5, fy) * inv_area;
        let mut w1 = edge(x2, y2, x0, y0, px as f32 + 0.5, fy) * inv_area;
        let mut w2 = 1.0 - w0 - w1;
        // Per-pixel increments (dx = +1)
        let dw0 = (y1 - y2) * inv_area;
        let dw1 = (y2 - y0) * inv_area;

        while px < max_x {
            if w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0 {
                let u = w0 * v0.uv.x + w1 * v1.uv.x + w2 * v2.uv.x;
                let v = w0 * v0.uv.y + w1 * v1.uv.y + w2 * v2.uv.y;
                let t = sample(tex, u, v);
                let c0 = v0.color;
                let c1 = v1.color;
                let c2 = v2.color;
                let cr = (w0 * c0.r() as f32 + w1 * c1.r() as f32 + w2 * c2.r() as f32) as u32;
                let cg = (w0 * c0.g() as f32 + w1 * c1.g() as f32 + w2 * c2.g() as f32) as u32;
                let cb = (w0 * c0.b() as f32 + w1 * c1.b() as f32 + w2 * c2.b() as f32) as u32;
                let ca = (w0 * c0.a() as f32 + w1 * c1.a() as f32 + w2 * c2.a() as f32) as u32;

                // src color = vertex * texture (straight), alpha = va * ta
                let sa = (ca * t[3] as u32 + 127) / 255;
                if sa != 0 {
                    let sr = (cr * t[0] as u32 + 127) / 255;
                    let sg = (cg * t[1] as u32 + 127) / 255;
                    let sb = (cb * t[2] as u32 + 127) / 255;
                    let idx = (py * dw + px) as usize;
                    let d = dst[idx];
                    let dr = (d >> 16) & 0xFF;
                    let dg = (d >> 8) & 0xFF;
                    let db = d & 0xFF;
                    let inv = 255 - sa;
                    let or = (sr * sa + dr * inv + 127) / 255;
                    let og = (sg * sa + dg * inv + 127) / 255;
                    let ob = (sb * sa + db * inv + 127) / 255;
                    dst[idx] = 0xFF00_0000 | (or << 16) | (og << 8) | ob;
                }
            }
            px += 1;
            w0 += dw0;
            w1 += dw1;
            w2 = 1.0 - w0 - w1;
        }
    }
}

#[inline]
fn sample(tex: &Texture, u: f32, v: f32) -> [u8; 4] {
    if tex.linear {
        sample_linear(tex, u, v)
    } else {
        sample_nearest(tex, u, v)
    }
}

#[inline]
fn sample_nearest(tex: &Texture, u: f32, v: f32) -> [u8; 4] {
    let x = (u * tex.w as f32 - 0.5).round() as i32;
    let y = (v * tex.h as f32 - 0.5).round() as i32;
    texel(tex, x, y)
}

fn sample_linear(tex: &Texture, u: f32, v: f32) -> [u8; 4] {
    let fx = u * tex.w as f32 - 0.5;
    let fy = v * tex.h as f32 - 0.5;
    let x0 = fx.floor() as i32;
    let y0 = fy.floor() as i32;
    let tx = ((fx - x0 as f32) * 256.0) as i32;
    let ty = ((fy - y0 as f32) * 256.0) as i32;
    let p00 = texel(tex, x0, y0);
    let p10 = texel(tex, x0 + 1, y0);
    let p01 = texel(tex, x0, y0 + 1);
    let p11 = texel(tex, x0 + 1, y0 + 1);
    let mut out = [0u8; 4];
    for i in 0..4 {
        let top = (p00[i] as i32 * (256 - tx) + p10[i] as i32 * tx) >> 8;
        let bot = (p01[i] as i32 * (256 - tx) + p11[i] as i32 * tx) >> 8;
        out[i] = ((top * (256 - ty) + bot * ty) >> 8) as u8;
    }
    out
}

#[inline]
fn texel(tex: &Texture, x: i32, y: i32) -> [u8; 4] {
    let x = x.clamp(0, tex.w as i32 - 1) as usize;
    let y = y.clamp(0, tex.h as i32 - 1) as usize;
    let i = (y * tex.w + x) * 4;
    [tex.rgba[i], tex.rgba[i + 1], tex.rgba[i + 2], tex.rgba[i + 3]]
}

/// Clear the whole backbuffer to an opaque color.
pub fn clear(dst: &mut [u32], c: Color32) {
    let px = 0xFF00_0000 | ((c.r() as u32) << 16) | ((c.g() as u32) << 8) | c.b() as u32;
    dst.fill(px);
}
