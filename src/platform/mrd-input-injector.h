#pragma once

#include <glib-object.h>
#include "../mrd-types.h"

G_BEGIN_DECLS

#define MRD_TYPE_INPUT_INJECTOR (mrd_input_injector_get_type ())
G_DECLARE_FINAL_TYPE (MrdInputInjector, mrd_input_injector, MRD, INPUT_INJECTOR, GObject)

MrdInputInjector *mrd_input_injector_new (void);

void mrd_input_injector_handle_keyboard (MrdInputInjector *injector,
                                         guint16           flags,
                                         guint8            scancode);

void mrd_input_injector_handle_mouse (MrdInputInjector *injector,
                                      guint16           flags,
                                      guint16           x,
                                      guint16           y);

void mrd_input_injector_handle_extended_mouse (MrdInputInjector *injector,
                                               guint16           flags,
                                               guint16           x,
                                               guint16           y);

/* Translates RDP-local (x,y) to global display coordinates. */
void mrd_input_injector_set_target_display (MrdInputInjector *injector,
                                            uint32_t          display_id);

/* Clears stuck modifier state from client sync/focus events. */
void mrd_input_injector_release_modifiers (MrdInputInjector *injector);

/* Scales mouse coords when client/VD logical sizes differ. */
void mrd_input_injector_set_client_size (MrdInputInjector *injector,
                                         uint32_t          width,
                                         uint32_t          height);

/* FALSE if cursor is off-display or target/client size not yet set. */
gboolean mrd_input_injector_get_client_cursor_position (MrdInputInjector *injector,
                                                        guint16          *out_x,
                                                        guint16          *out_y);

G_END_DECLS
