#ifndef __LSWTCS_MEDIA_H__
#define __LSWTCS_MEDIA_H__

extern void deinit_display(void);
extern int  init_display(int w, int h);
extern void get_display_size(int *w, int *h);
extern void flip_display_surface(void);
extern int  update_input(void);

/* Used by symtable_lswtcs.c's EGL stubs - we hand each fake EGLContext
 * a real SDL_GLContext so worker threads each own a real GL context
 * (sharing textures with the main one). */
extern int   sdl_gl_bind_main(void);
extern int   sdl_gl_bind(void *ctx);
extern void  sdl_gl_release_main(void);
extern void *sdl_gl_create_shared(void);
extern void  sdl_gl_destroy(void *ctx);
extern void  sdl_gl_swap(void);

#endif
