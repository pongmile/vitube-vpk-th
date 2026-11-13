#pragma once
#include <stdint.h>
#define RGBA8(r,g,b,a) (((unsigned)((a)&0xFF)<<24)|((unsigned)((b)&0xFF)<<16)|((unsigned)((g)&0xFF)<<8)|(unsigned)((r)&0xFF))
void vita2d_start_drawing(void); void vita2d_end_drawing(void); void vita2d_clear_screen(void);
void vita2d_wait_rendering_done(void); void vita2d_swap_buffers(void);
void vita2d_enable_clipping(void); void vita2d_disable_clipping(void);
void vita2d_set_clip_rectangle(int,int,int,int);
typedef struct vita2d_texture vita2d_texture;
typedef struct vita2d_font vita2d_font; typedef struct vita2d_pgf vita2d_pgf;
int vita2d_init(void); int vita2d_fini(void); void vita2d_free_font(vita2d_font*); void vita2d_free_pgf(vita2d_pgf*);
void *vita2d_get_context(void); void *vita2d_get_current_fb(void); vita2d_pgf *vita2d_load_default_pgf(void);
vita2d_font *vita2d_load_font_file(const char*); void vita2d_set_clear_color(unsigned int);
int vita2d_font_draw_text(vita2d_font*,int,int,unsigned int,unsigned int,const char*); int vita2d_font_text_width(vita2d_font*,unsigned int,const char*);
int vita2d_pgf_draw_text(vita2d_pgf*,int,int,unsigned int,float,const char*); int vita2d_pgf_text_width(vita2d_pgf*,float,const char*);
void vita2d_draw_rectangle(float,float,float,float,unsigned int);
void vita2d_draw_line(float,float,float,float,unsigned int); void vita2d_draw_fill_circle(float,float,float,unsigned int);
void vita2d_free_texture(vita2d_texture*); vita2d_texture *vita2d_load_JPEG_buffer(const void*, unsigned long);
int vita2d_texture_get_width(const vita2d_texture*); int vita2d_texture_get_height(const vita2d_texture*);
void vita2d_draw_texture_scale(const vita2d_texture*, float, float, float, float);
