#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_event.h>
#include <MagickWand/MagickWand.h>
#include <cairo.h>
#include <cairo/cairo-xcb.h>
#define MAXLEN 256
#define MAX(A, B)         ((A) > (B) ? (A) : (B))

// G L O B A L S //
static xcb_connection_t *dpy;
static xcb_screen_t *screen;
static int default_screen;
static xcb_window_t root;
static xcb_visualtype_t *visual;
static xcb_ewmh_connection_t *ewmh;
static uint32_t resolution[2];
static int run;

static int current_desktop;
static xcb_window_t focused_task;
static char original_image_path[MAXLEN] = {0};
static const char original_image_tmp_path[] = "/tmp.original_image.png";
static const char blurred_image_tmp_path[] = "/tmp/.blurred_image.png";
static xcb_pixmap_t original_background = XCB_NONE;
static xcb_pixmap_t blurred_background = XCB_NONE;
static uint32_t color_black = 0;

// D E F I N I T I O N S //

xcb_visualtype_t *get_root_visual_type(void);
xcb_pixmap_t create_bg_pixmap(const char *);
void handle_signal(int);
void register_root_events(void);
void get_blur_image_path(void);
void set_pixmap_background(xcb_pixmap_t);
void desktop_focus_change(void);
int task_focus_change(void);
int is_task_hidden(xcb_window_t);
int is_desktop_empty(void);
void cleanup(void);
void setup(void);
__attribute__((noreturn))
void err(char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}
xcb_visualtype_t *get_root_visual_type(void) {
  xcb_visualtype_t *visual_type = NULL;
  xcb_depth_iterator_t depth_iter;
  xcb_visualtype_iterator_t visual_iter;

  for (depth_iter = xcb_screen_allowed_depths_iterator(screen); depth_iter.rem; xcb_depth_next(&depth_iter)) {
    for (visual_iter = xcb_depth_visuals_iterator(depth_iter.data); visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
      if (screen->root_visual != visual_iter.data->visual_id)  continue;
      visual_type = visual_iter.data;
      return visual_type;
    }
  }
  return NULL;
}

xcb_pixmap_t create_bg_pixmap(const char *filename) {
  cairo_surface_t *img = cairo_image_surface_create_from_png(filename);
  xcb_pixmap_t bg_pixmap = xcb_generate_id(dpy);
  xcb_create_pixmap(dpy, screen->root_depth, bg_pixmap, root, resolution[0], resolution[1]);
  xcb_gcontext_t gc = xcb_generate_id(dpy);
  uint32_t values[] = {0};
  xcb_create_gc(dpy, gc, bg_pixmap, XCB_GC_FOREGROUND, values);
  xcb_rectangle_t rect = {0, 0, resolution[0], resolution[1]};
  xcb_poly_fill_rectangle(dpy, bg_pixmap, gc, 1, &rect);
  xcb_free_gc(dpy, gc);
  cairo_surface_t *xcb_output = cairo_xcb_surface_create(dpy, bg_pixmap, visual, resolution[0], resolution[1]);
  cairo_t *xcb_ctx = cairo_create(xcb_output); 
  cairo_set_source_surface(xcb_ctx, img, 0, 0);
  cairo_paint(xcb_ctx);
  cairo_surface_destroy(xcb_output);
  cairo_destroy(xcb_ctx);
  cairo_surface_destroy(img);
  return bg_pixmap;
}

void handle_signal(int sig) {
  if (sig == SIGTERM || sig == SIGINT || sig == SIGHUP)
    run = 0;	
}


void register_root_events(void) {
  uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};
  xcb_generic_error_t *err = xcb_request_check(dpy, xcb_change_window_attributes_checked(dpy, screen->root, XCB_CW_EVENT_MASK, values));
  if (err != NULL)  run = 0;	
}

void get_blur_image_path(void) {
    #define ThrowWandException(wand) \
    { \
      char \
        *description; \
     \
      ExceptionType \
        severity; \
     \
      description=MagickGetException(wand,&severity); \
      (void) fprintf(stderr,"%s %s %lu %s\n",GetMagickModule(),description); \
      description=(char *) MagickRelinquishMemory(description); \
      exit(-1); \
    }
    if (original_image_path){
        int i = 0;
        MagickWandGenesis();
        MagickBooleanType status;
        MagickWand *magick_wand;
        magick_wand = NewMagickWand();
        status=MagickReadImage(magick_wand, original_image_path);
        if (status == MagickFalse) ThrowWandException(magick_wand);
        status = MagickWriteImages(magick_wand,original_image_tmp_path,MagickTrue);
        if (status == MagickFalse) ThrowWandException(magick_wand);
        status = MagickBlurImage(magick_wand, 0, 8);
        if (status == MagickFalse) ThrowWandException(magick_wand);
        status=MagickWriteImages(magick_wand,blurred_image_tmp_path,MagickTrue);
        if (status == MagickFalse) ThrowWandException(magick_wand);
        magick_wand=DestroyMagickWand(magick_wand);
        MagickWandTerminus();
    }
}
void set_pixmap_background(xcb_pixmap_t bg_pixmap) {
    xcb_change_window_attributes(dpy, root, XCB_CW_BACK_PIXMAP, (uint32_t[1]){bg_pixmap});
    xcb_clear_area(dpy, 0, root, 0, 0, resolution[0], resolution[1]);
    xcb_flush(dpy);
} 
void desktop_focus_change(void) {
    if (xcb_ewmh_get_current_desktop_reply(ewmh, xcb_ewmh_get_current_desktop(ewmh, default_screen), &current_desktop, NULL) != 1){
        current_desktop = -1;
    }
}
int task_focus_change(void){
    if (xcb_ewmh_get_active_window_reply(ewmh, xcb_ewmh_get_active_window(ewmh, default_screen), &focused_task, NULL) == 1){
        uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};
        xcb_generic_error_t *err = xcb_request_check(dpy, xcb_change_window_attributes_checked(dpy, focused_task, XCB_CW_EVENT_MASK, values));
        if (err != NULL)  fprintf(stderr, "could not capture property change events on window 0x%X\n", focused_task);
        return 0;
    }
    return 1;	
}

int is_task_hidden(xcb_window_t win) {
    xcb_ewmh_get_atoms_reply_t ewmh_atoms_reply;
    if (xcb_ewmh_get_wm_state_reply(ewmh, xcb_ewmh_get_wm_state(ewmh, win), &ewmh_atoms_reply, NULL ) == 1){
        if (ewmh_atoms_reply.atoms_len > 0){
            for (int i=0; i < ewmh_atoms_reply.atoms_len;i++){
                if (ewmh_atoms_reply.atoms[i] == ewmh ->_NET_WM_STATE_HIDDEN) {
                    return 1;
                }
            }
        }
        xcb_ewmh_get_atoms_reply_wipe(&ewmh_atoms_reply);
    }
    return 0;
}
int is_desktop_empty(void) {
    int nbr_of_tasks = 0;
    uint32_t task_desktop;
    xcb_ewmh_get_windows_reply_t win_reply;xcb_window_t *wins = NULL;
    if (xcb_ewmh_get_client_list_reply(ewmh, xcb_ewmh_get_client_list(ewmh, default_screen), 
                                        &win_reply, NULL)  == 1) {
        nbr_of_tasks = win_reply.windows_len;
        wins = win_reply.windows;
        if (wins != NULL){
            for (int i = 0; i < nbr_of_tasks; i++) {
                
                if (xcb_ewmh_get_wm_desktop_reply(ewmh, xcb_ewmh_get_wm_desktop(ewmh, wins[i]), &task_desktop, NULL) == 1) {
                    if (task_desktop == current_desktop){
                        if (is_task_hidden(wins[i])) continue;
                        xcb_ewmh_get_windows_reply_wipe(&win_reply);
                        return 0;
                    }
                }
            }
        }
        xcb_ewmh_get_windows_reply_wipe(&win_reply);		
    }
    return 1;	
}
void cleanup(void) {
    xcb_ewmh_connection_wipe(ewmh);
    free(ewmh);
    xcb_free_pixmap(dpy, original_background);
    xcb_free_pixmap(dpy, blurred_background);
    xcb_disconnect(dpy);	
}

void setup(void) {
    dpy = xcb_connect(NULL, &default_screen);
    if (xcb_connection_has_error(dpy))
        err("Can't open display.\n");
    screen = xcb_setup_roots_iterator(xcb_get_setup(dpy)).data;
    if (screen == NULL)
        err("Can't acquire screen.\n");
    root = screen->root;
    resolution[0] = screen -> width_in_pixels;
    resolution[1] = screen -> height_in_pixels;
    ewmh = malloc(sizeof(xcb_ewmh_connection_t));
    if (xcb_ewmh_init_atoms_replies(ewmh, xcb_ewmh_init_atoms(dpy, ewmh), NULL) == 0)
        err("Can't initialize EWMH atoms.\n");
    visual = get_root_visual_type();
    if (visual == NULL)
        err("Can't find root visual.\n");
    get_blur_image_path();
    original_background = create_bg_pixmap(original_image_tmp_path);
    blurred_background = create_bg_pixmap(blurred_image_tmp_path);
    
}
int main(int argc, char*argv[]) {
    if (argc != 3){
        err("Usage ./blur -i /path/to/image_file");
        return 1;
    }
    ++argv;
    if (strcmp(*argv, "-i") != 0){
        err("Usage ./blur -i /path/to/image_file");
        return 1;
    }
    ++argv;
    strcat(original_image_path, *argv);
    
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGHUP, handle_signal);
    run = 1;
    xcb_generic_event_t *evt;
    setup();
    set_pixmap_background(original_background);
    register_root_events();
    int dpy_fd = xcb_get_file_descriptor(dpy);
    int sel_fd = MAX(dpy_fd, -1) + 1;
    fd_set descriptors;
    while (run){
        FD_ZERO(&descriptors);
        FD_SET(dpy_fd, &descriptors);
        if (select(sel_fd, &descriptors, NULL, NULL, NULL)) {
            if (FD_ISSET(dpy_fd, &descriptors))
                while ((evt = xcb_poll_for_event(dpy)) != NULL) {
                    xcb_property_notify_event_t *pne;
                    switch (XCB_EVENT_RESPONSE_TYPE(evt)) {
                        case XCB_PROPERTY_NOTIFY:
                            pne = (xcb_property_notify_event_t *) evt;
                            if (pne->atom == ewmh->_NET_CURRENT_DESKTOP) {
                                desktop_focus_change();
                                if (is_desktop_empty()){
                                    set_pixmap_background(original_background);
                                }
                                else{
                                    set_pixmap_background(blurred_background);
                                }
                            } else 
                            if (pne->atom == ewmh->_NET_CLIENT_LIST){
                                if (is_desktop_empty()){
                                    set_pixmap_background(original_background);
                                }
                                else{
                                    set_pixmap_background(blurred_background);
                                }
                            } else
                            if (pne->atom == ewmh -> _NET_ACTIVE_WINDOW){
                                if (task_focus_change()){
                                    set_pixmap_background(blurred_background);
                                }
                            }else 
                            if (pne->window != screen->root && pne->atom == ewmh->_NET_WM_STATE){
                                if (is_desktop_empty()){
                                    set_pixmap_background(original_background);
                                }
                                else{
                                    set_pixmap_background(blurred_background);
                                }
                            }
                        default:
                            free(evt);
                            continue;
                    }
                    free(evt);
                }
            }
    }
    set_pixmap_background(original_background);
    cleanup();
    return 0;
    
}
