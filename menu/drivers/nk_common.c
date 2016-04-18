/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2016 - Daniel De Matteis
 *  Copyright (C) 2014-2015 - Jean-Andr� Santoni
 *  Copyright (C) 2016      - Andr�s Su�rez
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <streams/file_stream.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION

#include "nk_common.h"

#include "../menu_display.h"
#include "../../gfx/video_shader_driver.h"

#include "../../gfx/drivers/gl_shaders/pipeline_zahnrad.glsl.vert.h"
#include "../../gfx/drivers/gl_shaders/pipeline_zahnrad.glsl.frag.h"

struct nk_font font;
struct nk_user_font usrfnt;
struct nk_allocator nk_alloc;
struct nk_device device;

struct nk_image nk_common_image_load(const char *filename)
{
    int x,y,n;
    GLuint tex;
    unsigned char *data = stbi_load(filename, &x, &y, &n, 0);
    if (!data) printf("Failed to load image: %s\n", filename);

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, x, y, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
#endif

    stbi_image_free(data);
    return nk_image_id((int)tex);
}

char* nk_common_file_load(const char* path, size_t* size)
{
   void *buf;
   ssize_t *length = (ssize_t*)size;
   filestream_read_file(path, &buf, length);
   return (char*)buf;
}

NK_API void nk_common_device_init(struct nk_device *dev)
{
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   GLint status;

   dev->prog      = glCreateProgram();
   dev->vert_shdr = glCreateShader(GL_VERTEX_SHADER);
   dev->frag_shdr = glCreateShader(GL_FRAGMENT_SHADER);
   glShaderSource(dev->vert_shdr, 1, &zahnrad_vertex_shader, 0);
   glShaderSource(dev->frag_shdr, 1, &zahnrad_fragment_shader, 0);
   glCompileShader(dev->vert_shdr);
   glCompileShader(dev->frag_shdr);
   glGetShaderiv(dev->vert_shdr, GL_COMPILE_STATUS, &status);
   glGetShaderiv(dev->frag_shdr, GL_COMPILE_STATUS, &status);
   glAttachShader(dev->prog, dev->vert_shdr);
   glAttachShader(dev->prog, dev->frag_shdr);
   glLinkProgram(dev->prog);
   glGetProgramiv(dev->prog, GL_LINK_STATUS, &status);

   dev->uniform_proj = glGetUniformLocation(dev->prog, "ProjMtx");
   dev->attrib_pos   = glGetAttribLocation(dev->prog, "Position");
   dev->attrib_uv    = glGetAttribLocation(dev->prog, "TexCoord");
   dev->attrib_col   = glGetAttribLocation(dev->prog, "Color");

   {
      /* buffer setup */
      GLsizei vs = sizeof(struct nk_draw_vertex);
      size_t vp = offsetof(struct nk_draw_vertex, position);
      size_t vt = offsetof(struct nk_draw_vertex, uv);
      size_t vc = offsetof(struct nk_draw_vertex, col);

      glGenBuffers(1, &dev->vbo);
      glGenBuffers(1, &dev->ebo);
      glGenVertexArrays(1, &dev->vao);

      glBindVertexArray(dev->vao);
      glBindBuffer(GL_ARRAY_BUFFER, dev->vbo);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dev->ebo);

      glEnableVertexAttribArray((GLuint)dev->attrib_pos);
      glEnableVertexAttribArray((GLuint)dev->attrib_uv);
      glEnableVertexAttribArray((GLuint)dev->attrib_col);

      glVertexAttribPointer((GLuint)dev->attrib_pos, 2, GL_FLOAT, GL_FALSE, vs, (void*)vp);
      glVertexAttribPointer((GLuint)dev->attrib_uv, 2, GL_FLOAT, GL_FALSE, vs, (void*)vt);
      glVertexAttribPointer((GLuint)dev->attrib_col, 4, GL_UNSIGNED_BYTE, GL_TRUE, vs, (void*)vc);
   }

   glBindTexture(GL_TEXTURE_2D, 0);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
   glBindVertexArray(0);
#endif
}

struct nk_user_font nk_common_font(
      struct nk_device *dev,
      struct nk_font *font,
      const char *path,
      unsigned int font_height,
      const nk_rune *range)
{
   int glyph_count;
   int img_width, img_height;
   struct nk_font_glyph *glyphes;
   struct nk_baked_font baked_font;
   struct nk_user_font user_font;
   struct nk_recti custom;

   memset(&baked_font, 0, sizeof(baked_font));
   memset(&user_font, 0, sizeof(user_font));
   memset(&custom, 0, sizeof(custom));

   {
      struct texture_image ti;
      /* bake and upload font texture */
      struct nk_font_config config;
      void *img, *tmp;
      size_t ttf_size;
      size_t tmp_size, img_size;
      const char *custom_data = "....";
      char *ttf_blob = nk_common_file_load(path, &ttf_size);
       /* setup font configuration */
      memset(&config, 0, sizeof(config));

      config.ttf_blob     = ttf_blob;
      config.ttf_size     = ttf_size;
      config.font         = &baked_font;
      config.coord_type   = NK_COORD_UV;
      config.range        = range;
      config.pixel_snap   = nk_false;
      config.size         = (float)font_height;
      config.spacing      = nk_vec2(0,0);
      config.oversample_h = 1;
      config.oversample_v = 1;

      /* query needed amount of memory for the font baking process */
      nk_font_bake_memory(&tmp_size, &glyph_count, &config, 1);
      glyphes = (struct nk_font_glyph*)
         calloc(sizeof(struct nk_font_glyph), (size_t)glyph_count);
      tmp = calloc(1, tmp_size);

      /* pack all glyphes and return needed image width, height and memory size*/
      custom.w = 2; custom.h = 2;
      nk_font_bake_pack(&img_size,
            &img_width,&img_height,&custom,tmp,tmp_size,&config, 1, &nk_alloc);

      /* bake all glyphes and custom white pixel into image */
      img = calloc(1, img_size);
      nk_font_bake(img, img_width,
            img_height, tmp, tmp_size, glyphes, glyph_count, &config, 1);
      nk_font_bake_custom_data(img,
            img_width, img_height, custom, custom_data, 2, 2, '.', 'X');

      {
         /* convert alpha8 image into rgba8 image */
         void *img_rgba = calloc(4, (size_t)(img_height * img_width));
         nk_font_bake_convert(img_rgba, img_width, img_height, img);
         free(img);
         img = img_rgba;
      }

      /* upload baked font image */
      ti.pixels = (uint32_t*)img;
      ti.width  = (GLsizei)img_width;
      ti.height = (GLsizei)img_height;

      video_driver_texture_load(&ti,
            TEXTURE_FILTER_MIPMAP_NEAREST, (uintptr_t*)&dev->font_tex);

      free(ttf_blob);
      free(tmp);
      free(img);
   }

   /* default white pixel in a texture which is needed to draw primitives */
   dev->null.texture.id = (int)dev->font_tex;
   dev->null.uv = nk_vec2((custom.x + 0.5f)/(float)img_width,
      (custom.y + 0.5f)/(float)img_height);

   /* setup font with glyphes. IMPORTANT: the font only references the glyphes
      this was done to have the possibility to have multible fonts with one
      total glyph array. Not quite sure if it is a good thing since the
      glyphes have to be freed as well. */
   nk_font_init(font,
         (float)font_height, '?', glyphes,
         &baked_font, dev->null.texture);
   user_font = font->handle;
   return user_font;
}

void nk_common_device_shutdown(struct nk_device *dev)
{
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   glDetachShader(dev->prog, dev->vert_shdr);
   glDetachShader(dev->prog, dev->frag_shdr);
   glDeleteShader(dev->vert_shdr);
   glDeleteShader(dev->frag_shdr);
   glDeleteProgram(dev->prog);
   glDeleteTextures(1, &dev->font_tex);
   glDeleteBuffers(1, &dev->vbo);
   glDeleteBuffers(1, &dev->ebo);
#endif
}

void nk_common_device_draw(struct nk_device *dev,
      struct nk_context *ctx, int width, int height,
      enum nk_anti_aliasing AA)
{
   video_shader_ctx_info_t shader_info;
   struct nk_buffer vbuf, ebuf;
   struct nk_convert_config config;
   uintptr_t                 last_prog;
   const struct nk_draw_command *cmd = NULL;
   void                    *vertices = NULL;
   void                    *elements = NULL;
   const nk_draw_index       *offset = NULL;
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   GLint last_tex;
   GLint last_ebo, last_vbo, last_vao;
   GLfloat ortho[4][4] = {
      {2.0f, 0.0f, 0.0f, 0.0f},
      {0.0f,-2.0f, 0.0f, 0.0f},
      {0.0f, 0.0f,-1.0f, 0.0f},
      {-1.0f,1.0f, 0.0f, 1.0f},
   };
   ortho[0][0] /= (GLfloat)width;
   ortho[1][1] /= (GLfloat)height;

   /* save previous opengl state */
   glGetIntegerv(GL_CURRENT_PROGRAM, (GLint*)&last_prog);
   glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);
   glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_vao);
   glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_ebo);
   glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vbo);
#endif

   menu_display_ctl(MENU_DISPLAY_CTL_BLEND_BEGIN, NULL);

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   glActiveTexture(GL_TEXTURE0);
#endif

   /* setup program */
   shader_info.data       = NULL;
   shader_info.idx        = dev->prog;
   shader_info.set_active = false;
   video_shader_driver_ctl(SHADER_CTL_USE, &shader_info);

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   glUniformMatrix4fv(dev->uniform_proj, 1, GL_FALSE, &ortho[0][0]);

   /* convert from command queue into draw list and draw to screen */

   /* allocate vertex and element buffer */
   glBindVertexArray(dev->vao);
   glBindBuffer(GL_ARRAY_BUFFER, dev->vbo);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dev->ebo);

   glBufferData(GL_ARRAY_BUFFER, MAX_VERTEX_MEMORY, NULL, GL_STREAM_DRAW);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, MAX_ELEMENT_MEMORY, NULL, GL_STREAM_DRAW);

   /* load draw vertices & elements directly into vertex + element buffer */
   vertices = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
   elements = glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_WRITE_ONLY);
#endif

   /* fill converting configuration */
   memset(&config, 0, sizeof(config));

   config.global_alpha         = 1.0f;
   config.shape_AA             = AA;
   config.line_AA              = AA;
   config.circle_segment_count = 22;
   //config.line_thickness       = 1.0f;
   config.null                 = dev->null;

   /* setup buffers to load vertices and elements */
   nk_buffer_init_fixed(&vbuf, vertices, MAX_VERTEX_MEMORY);
   nk_buffer_init_fixed(&ebuf, elements, MAX_ELEMENT_MEMORY);
   nk_convert(ctx, &dev->cmds, &vbuf, &ebuf, &config);

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   glUnmapBuffer(GL_ARRAY_BUFFER);
   glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
#endif

   /* iterate over and execute each draw command */
   nk_draw_foreach(cmd, ctx, &dev->cmds)
   {
      if (!cmd->elem_count)
         continue;

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
      glBindTexture(GL_TEXTURE_2D, (GLuint)cmd->texture.id);
      glScissor((GLint)cmd->clip_rect.x,
            height - (GLint)(cmd->clip_rect.y + cmd->clip_rect.h),
            (GLint)cmd->clip_rect.w, (GLint)cmd->clip_rect.h);
      glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count,
            GL_UNSIGNED_SHORT, offset);
#endif

      offset += cmd->elem_count;
   }
   nk_clear(ctx);

   /* restore old state */
   shader_info.data       = NULL;
   shader_info.idx        = (GLint)last_prog;
   shader_info.set_active = false;
   video_shader_driver_ctl(SHADER_CTL_USE, &shader_info);

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   glBindTexture(GL_TEXTURE_2D, (GLuint)last_tex);
   glBindBuffer(GL_ARRAY_BUFFER, (GLuint)last_vbo);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)last_ebo);
   glBindVertexArray((GLuint)last_vao);
#endif

   menu_display_ctl(MENU_DISPLAY_CTL_BLEND_END, NULL);
}

void* nk_common_mem_alloc(nk_handle unused, size_t size)
{
   (void)unused;
   return calloc(1, size);
}

void nk_common_mem_free(nk_handle unused, void *ptr)
{
   (void)unused;
   free(ptr);
}
