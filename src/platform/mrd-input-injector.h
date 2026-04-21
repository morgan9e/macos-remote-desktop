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

void mrd_input_injector_set_target_display (MrdInputInjector *injector,
                                            uint32_t          display_id);

void mrd_input_injector_release_modifiers (MrdInputInjector *injector);

void mrd_input_injector_set_client_size (MrdInputInjector *injector,
                                         uint32_t          width,
                                         uint32_t          height);
G_END_DECLS