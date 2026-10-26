/* Minimal vita2d declarations for host syntax checks only. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct vita2d_pgf vita2d_pgf;
typedef struct vita2d_font vita2d_font;
typedef struct vita2d_texture vita2d_texture;
typedef struct vita2d_color_vertex { float x, y, z; unsigned int color; } vita2d_color_vertex;
typedef enum { SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_PRIMITIVE_TRIANGLE_FAN } SceGxmPrimitiveType;
void vita2d_start_drawing(void); void vita2d_end_drawing(void); void vita2d_clear_screen(void);
void vita2d_swap_buffers(void); void vita2d_wait_rendering_done(void); int vita2d_common_dialog_update(void);
void vita2d_set_clear_color(unsigned int color);
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color);
void vita2d_draw_fill_circle(float x, float y, float radius, unsigned int color);
void* vita2d_pool_memalign(unsigned int size, unsigned int alignment);
void vita2d_draw_array(SceGxmPrimitiveType mode, const vita2d_color_vertex* vertices, size_t count);
vita2d_pgf* vita2d_load_default_pgf(void); void vita2d_free_pgf(vita2d_pgf* f);
int vita2d_pgf_draw_text(vita2d_pgf* font, int x, int y, unsigned int color, float scale, const char* text);
int vita2d_pgf_text_width(vita2d_pgf* font, float scale, const char* text);
vita2d_font* vita2d_load_font_file(const char* filename); void vita2d_free_font(vita2d_font* font);
int vita2d_font_draw_text(vita2d_font* font, int x, int y, unsigned int color, unsigned int size, const char* text);
int vita2d_font_text_width(vita2d_font* font, unsigned int size, const char* text);
enum { SCE_GXM_TEXTURE_FILTER_POINT = 0, SCE_GXM_TEXTURE_FILTER_LINEAR = 1 };
vita2d_texture* vita2d_create_empty_texture(unsigned int w, unsigned int h);
void* vita2d_texture_get_datap(const vita2d_texture* t); unsigned int vita2d_texture_get_stride(const vita2d_texture* t);
void vita2d_texture_set_filters(const vita2d_texture* t, int min, int mag);
void vita2d_draw_texture_scale(const vita2d_texture* t, float x, float y, float xs, float ys);
void vita2d_set_clip_rectangle(int x0, int y0, int x1, int y1); void vita2d_enable_clipping(void); void vita2d_disable_clipping(void);
