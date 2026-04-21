#include "mrd-input-injector.h"

#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>

#define KBD_FLAGS_EXTENDED   0x0100
#define KBD_FLAGS_DOWN       0x4000
#define KBD_FLAGS_RELEASE    0x8000

#define PTR_FLAGS_MOVE       0x0800
#define PTR_FLAGS_DOWN       0x8000
#define PTR_FLAGS_BUTTON1    0x1000  /* Left */
#define PTR_FLAGS_BUTTON2    0x2000  /* Right */
#define PTR_FLAGS_BUTTON3    0x4000  /* Middle */
#define PTR_FLAGS_WHEEL      0x0200
#define PTR_FLAGS_HWHEEL     0x0400
#define PTR_FLAGS_WHEEL_NEGATIVE 0x0100

#define PTR_XFLAGS_DOWN      0x8000
#define PTR_XFLAGS_BUTTON1   0x0001  /* Button 4 */
#define PTR_XFLAGS_BUTTON2   0x0002  /* Button 5 */

struct _MrdInputInjector
{
  GObject parent;

  CGPoint last_mouse_position;  /* global coords */
  guint32 pressed_buttons;
  uint32_t target_display_id;   /* 0 = primary */

  uint32_t client_width;
  uint32_t client_height;

  int64_t last_click_time_us[3];   /* L, R, M */
  CGPoint last_click_pos[3];
  int64_t click_count[3];

  /* Tracked locally to force-sync CGEvent flags (prevents stuck modifiers). */
  CGEventFlags tracked_flags;

  /* Drops stray UPs from mstsc focus-sync / fastpath+slowpath dupes. */
  guint8 pressed[256];
};

G_DEFINE_TYPE (MrdInputInjector, mrd_input_injector, G_TYPE_OBJECT)

static CGPoint
translate_to_global (MrdInputInjector *injector, guint16 x, guint16 y)
{
  if (injector->target_display_id == 0)
    return CGPointMake (x, y);

  CGRect bounds = CGDisplayBounds (injector->target_display_id);
  if (CGRectIsNull (bounds) || CGRectIsEmpty (bounds))
    {
      static gboolean warned = FALSE;
      if (!warned)
        {
          g_warning ("Target display %u not active; falling back to global coords",
                     injector->target_display_id);
          warned = TRUE;
        }
      return CGPointMake (x, y);
    }

  CGFloat sx = (CGFloat)x;
  CGFloat sy = (CGFloat)y;
  if (injector->client_width > 0 && injector->client_height > 0)
    {
      sx = sx * bounds.size.width  / injector->client_width;
      sy = sy * bounds.size.height / injector->client_height;
    }

  CGFloat gx = bounds.origin.x + sx;
  CGFloat gy = bounds.origin.y + sy;
  /* Clamp inclusive of far edge — macOS tiling needs exact-edge cursor. */
  gx = MAX (bounds.origin.x, MIN (gx, bounds.origin.x + bounds.size.width));
  gy = MAX (bounds.origin.y, MIN (gy, bounds.origin.y + bounds.size.height));
  return CGPointMake (gx, gy);
}

/*
 * Scancode to macOS keycode mapping.
 *
 *   Windows Alt   → Mac Command
 *   Windows Win   → Mac Option 
 *   Windows Ctrl  → Mac Control
 *
 * Reference: MS-RDPBCGR scancode set 1 + HIToolbox/Events.h virtual keycodes
 */
static CGKeyCode
scancode_to_keycode (guint8 scancode, gboolean extended)
{
  /* Basic scancodes (no E0 prefix) */
  static const CGKeyCode basic_map[128] = {
    /* 0x00 */ 0xFF,
    /* 0x01 */ kVK_Escape,
    /* 0x02 */ kVK_ANSI_1,
    /* 0x03 */ kVK_ANSI_2,
    /* 0x04 */ kVK_ANSI_3,
    /* 0x05 */ kVK_ANSI_4,
    /* 0x06 */ kVK_ANSI_5,
    /* 0x07 */ kVK_ANSI_6,
    /* 0x08 */ kVK_ANSI_7,
    /* 0x09 */ kVK_ANSI_8,
    /* 0x0A */ kVK_ANSI_9,
    /* 0x0B */ kVK_ANSI_0,
    /* 0x0C */ kVK_ANSI_Minus,
    /* 0x0D */ kVK_ANSI_Equal,
    /* 0x0E */ kVK_Delete,            /* Backspace */
    /* 0x0F */ kVK_Tab,
    /* 0x10 */ kVK_ANSI_Q,
    /* 0x11 */ kVK_ANSI_W,
    /* 0x12 */ kVK_ANSI_E,
    /* 0x13 */ kVK_ANSI_R,
    /* 0x14 */ kVK_ANSI_T,
    /* 0x15 */ kVK_ANSI_Y,
    /* 0x16 */ kVK_ANSI_U,
    /* 0x17 */ kVK_ANSI_I,
    /* 0x18 */ kVK_ANSI_O,
    /* 0x19 */ kVK_ANSI_P,
    /* 0x1A */ kVK_ANSI_LeftBracket,
    /* 0x1B */ kVK_ANSI_RightBracket,
    /* 0x1C */ kVK_Return,
    /* 0x1D */ kVK_Control,           /* Left Ctrl */
    /* 0x1E */ kVK_ANSI_A,
    /* 0x1F */ kVK_ANSI_S,
    /* 0x20 */ kVK_ANSI_D,
    /* 0x21 */ kVK_ANSI_F,
    /* 0x22 */ kVK_ANSI_G,
    /* 0x23 */ kVK_ANSI_H,
    /* 0x24 */ kVK_ANSI_J,
    /* 0x25 */ kVK_ANSI_K,
    /* 0x26 */ kVK_ANSI_L,
    /* 0x27 */ kVK_ANSI_Semicolon,
    /* 0x28 */ kVK_ANSI_Quote,
    /* 0x29 */ kVK_ANSI_Grave,        /* ` ~ */
    /* 0x2A */ kVK_Shift,             /* Left Shift */
    /* 0x2B */ kVK_ANSI_Backslash,
    /* 0x2C */ kVK_ANSI_Z,
    /* 0x2D */ kVK_ANSI_X,
    /* 0x2E */ kVK_ANSI_C,
    /* 0x2F */ kVK_ANSI_V,
    /* 0x30 */ kVK_ANSI_B,
    /* 0x31 */ kVK_ANSI_N,
    /* 0x32 */ kVK_ANSI_M,
    /* 0x33 */ kVK_ANSI_Comma,
    /* 0x34 */ kVK_ANSI_Period,
    /* 0x35 */ kVK_ANSI_Slash,
    /* 0x36 */ kVK_RightShift,
    /* 0x37 */ kVK_ANSI_KeypadMultiply,
    /* 0x38 */ kVK_Command,           /* Left Alt → Mac Command */
    /* 0x39 */ kVK_Space,
    /* 0x3A */ kVK_CapsLock,
    /* 0x3B */ kVK_F1,
    /* 0x3C */ kVK_F2,
    /* 0x3D */ kVK_F3,
    /* 0x3E */ kVK_F4,
    /* 0x3F */ kVK_F5,
    /* 0x40 */ kVK_F6,
    /* 0x41 */ kVK_F7,
    /* 0x42 */ kVK_F8,
    /* 0x43 */ kVK_F9,
    /* 0x44 */ kVK_F10,
    /* 0x45 */ kVK_ANSI_KeypadClear,  /* Num Lock → Clear (Mac numpad behavior) */
    /* 0x46 */ kVK_F14,               /* Scroll Lock → F14 (no Mac equivalent) */
    /* 0x47 */ kVK_ANSI_Keypad7,
    /* 0x48 */ kVK_ANSI_Keypad8,
    /* 0x49 */ kVK_ANSI_Keypad9,
    /* 0x4A */ kVK_ANSI_KeypadMinus,
    /* 0x4B */ kVK_ANSI_Keypad4,
    /* 0x4C */ kVK_ANSI_Keypad5,
    /* 0x4D */ kVK_ANSI_Keypad6,
    /* 0x4E */ kVK_ANSI_KeypadPlus,
    /* 0x4F */ kVK_ANSI_Keypad1,
    /* 0x50 */ kVK_ANSI_Keypad2,
    /* 0x51 */ kVK_ANSI_Keypad3,
    /* 0x52 */ kVK_ANSI_Keypad0,
    /* 0x53 */ kVK_ANSI_KeypadDecimal,
    /* 0x54 */ 0xFF,                  /* SysRq (Alt+PrintScreen) */
    /* 0x55 */ 0xFF,
    /* 0x56 */ kVK_ISO_Section,       /* OEM 102 key (non-US backslash) */
    /* 0x57 */ kVK_F11,
    /* 0x58 */ kVK_F12,
    /* 0x59 */ kVK_ANSI_KeypadEquals, /* Keypad = */
    /* 0x5A */ 0xFF,
    /* 0x5B */ 0xFF,
    /* 0x5C */ 0xFF,
    /* 0x5D */ 0xFF,
    /* 0x5E */ 0xFF,
    /* 0x5F */ 0xFF,
    /* 0x60 */ 0xFF,
    /* 0x61 */ 0xFF,
    /* 0x62 */ 0xFF,
    /* 0x63 */ 0xFF,
    /* 0x64 */ kVK_F13,
    /* 0x65 */ kVK_F14,
    /* 0x66 */ kVK_F15,
    /* 0x67 */ kVK_F16,
    /* 0x68 */ kVK_F17,
    /* 0x69 */ kVK_F18,
    /* 0x6A */ kVK_F19,
    /* 0x6B */ kVK_F20,
    /* 0x6C */ 0xFF,                  /* F21 (no Mac equivalent) */
    /* 0x6D */ 0xFF,                  /* F22 */
    /* 0x6E */ 0xFF,                  /* F23 */
    /* 0x6F */ 0xFF,                  /* F24 */
    /* 0x70 */ 0xFF,                  /* Hiragana (Japanese) */
    /* 0x71 */ 0xFF,
    /* 0x72 */ 0xFF,
    /* 0x73 */ kVK_JIS_Underscore,    /* Japanese _ */
    /* 0x74 */ 0xFF,
    /* 0x75 */ 0xFF,
    /* 0x76 */ 0xFF,
    /* 0x77 */ 0xFF,
    /* 0x78 */ 0xFF,
    /* 0x79 */ kVK_JIS_KeypadComma,   /* Japanese numpad comma */
    /* 0x7A */ 0xFF,
    /* 0x7B */ kVK_JIS_Yen,           /* Japanese Yen */
    /* 0x7C */ 0xFF,
    /* 0x7D */ 0xFF,
    /* 0x7E */ 0xFF,
    /* 0x7F */ 0xFF,                  /* Pause (handled separately) */
  };

  /* Extended keys (with 0xE0 prefix) */
  if (extended) {
    switch (scancode) {
      /* Editing / navigation cluster */
      case 0x1C: return kVK_ANSI_KeypadEnter;
      case 0x1D: return kVK_RightControl;
      case 0x35: return kVK_ANSI_KeypadDivide;
      case 0x38: return kVK_RightCommand;  /* Right Alt → Mac Right Command */
      case 0x47: return kVK_Home;
      case 0x48: return kVK_UpArrow;
      case 0x49: return kVK_PageUp;
      case 0x4B: return kVK_LeftArrow;
      case 0x4D: return kVK_RightArrow;
      case 0x4F: return kVK_End;
      case 0x50: return kVK_DownArrow;
      case 0x51: return kVK_PageDown;
      case 0x52: return kVK_Help;          /* Insert → Help (Mac convention) */
      case 0x53: return kVK_ForwardDelete;

      /* Windows / Meta keys → Mac Option */
      case 0x5B: return kVK_Option;        /* Left Windows → Mac Left Option */
      case 0x5C: return kVK_RightOption;   /* Right Windows → Mac Right Option */

      /* Context menu key */
      case 0x5D: return 0x6E;              /* Menu key → kVK_ContextualMenuKey (0x6E) */

      /* Power / sleep (typically blocked by OS, but map them) */
      case 0x5E: return 0xFF;              /* Power */
      case 0x5F: return 0xFF;              /* Sleep */
      case 0x63: return 0xFF;              /* Wake */

      /* Media keys */
      case 0x10: return 0xFF;              /* Previous track (needs NX_KEYTYPE) */
      case 0x19: return 0xFF;              /* Next track */
      case 0x20: return 0xFF;              /* Mute */
      case 0x21: return 0xFF;              /* Calculator */
      case 0x22: return 0xFF;              /* Play/Pause */
      case 0x24: return 0xFF;              /* Stop */
      case 0x2E: return 0xFF;              /* Volume down */
      case 0x30: return 0xFF;              /* Volume up */
      case 0x32: return 0xFF;              /* Browser home */

      /* Print Screen (non-extended 0x37 is keypad multiply) */
      case 0x37: return kVK_F13;           /* Print Screen → F13 */

      /* Pause/Break - special: pause is E1 1D 45, but often comes as E0 */
      case 0x46: return kVK_F15;           /* Pause → F15 */

      default: return 0xFF;
    }
  }

  if (scancode < 128)
    return basic_map[scancode];

  return 0xFF;
}

/* macOS double/triple-click requires incremented clickState at same loc. */
static void
post_mouse_button (MrdInputInjector *injector,
                   int               button_idx,   /* 0=L, 1=R, 2=M */
                   CGEventType       type,
                   CGPoint           pos,
                   CGMouseButton     button,
                   gboolean          is_down)
{
  int64_t click_count = 1;

  if (is_down)
    {
      int64_t now_us = g_get_monotonic_time ();
      int64_t dt_us = now_us - injector->last_click_time_us[button_idx];
      CGPoint last_pos = injector->last_click_pos[button_idx];
      CGFloat dx = pos.x - last_pos.x;
      CGFloat dy = pos.y - last_pos.y;

      /* 500ms window, 5px tolerance (macOS default is ~500ms, ~5px) */
      if (dt_us < 500000 && (dx * dx + dy * dy) < 25.0)
        injector->click_count[button_idx]++;
      else
        injector->click_count[button_idx] = 1;

      injector->last_click_time_us[button_idx] = now_us;
      injector->last_click_pos[button_idx] = pos;
    }

  click_count = injector->click_count[button_idx];

  CGEventRef event = CGEventCreateMouseEvent (NULL, type, pos, button);
  if (event) {
    CGEventSetIntegerValueField (event, kCGMouseEventClickState, click_count);
    CGEventSetFlags (event, injector->tracked_flags);
    CGEventPost (kCGHIDEventTap, event);
    CFRelease (event);
  }
}

MrdInputInjector *
mrd_input_injector_new (void)
{
  return g_object_new (MRD_TYPE_INPUT_INJECTOR, NULL);
}

static CGEventFlags
keycode_to_modifier_flag (CGKeyCode keycode)
{
  switch (keycode) {
    case kVK_Control:
    case kVK_RightControl:   return kCGEventFlagMaskControl;
    case kVK_Shift:
    case kVK_RightShift:     return kCGEventFlagMaskShift;
    case kVK_Option:
    case kVK_RightOption:    return kCGEventFlagMaskAlternate;
    case kVK_Command:
    case kVK_RightCommand:   return kCGEventFlagMaskCommand;
    case kVK_Function:       return kCGEventFlagMaskSecondaryFn;
    default:                 return 0;
  }
}

void
mrd_input_injector_handle_keyboard (MrdInputInjector *injector,
                                    guint16           flags,
                                    guint8            scancode)
{
  g_return_if_fail (MRD_IS_INPUT_INJECTOR (injector));

  gboolean extended = (flags & KBD_FLAGS_EXTENDED) != 0;
  gboolean key_down = (flags & KBD_FLAGS_RELEASE) == 0;

  CGKeyCode keycode = scancode_to_keycode (scancode, extended);
  if (keycode == 0xFF) {
    g_message ("Key: unknown scancode 0x%02X (ext=%d)", scancode, extended);
    return;
  }

  /* Drop stray UPs from focus-sync / fastpath+slowpath dupes. */
  if (!key_down && !injector->pressed[keycode & 0xFF]) {
    g_debug ("Key: dropping stray UP scancode=0x%02X ext=%d keycode=%u",
             scancode, extended, keycode);
    return;
  }
  injector->pressed[keycode & 0xFF] = key_down ? 1 : 0;

  CGEventFlags mod_flag = keycode_to_modifier_flag (keycode);
  if (mod_flag) {
    if (key_down)
      injector->tracked_flags |= mod_flag;
    else
      injector->tracked_flags &= ~mod_flag;
  }

  CGEventRef event = CGEventCreateKeyboardEvent (NULL, keycode, key_down);
  if (event) {
    /* Force-sync flags to prevent stuck modifiers from missed up/downs. */
    CGEventSetFlags (event, injector->tracked_flags);
    CGEventPost (kCGHIDEventTap, event);
    CFRelease (event);
  }
}

void
mrd_input_injector_handle_mouse (MrdInputInjector *injector,
                                 guint16           flags,
                                 guint16           x,
                                 guint16           y)
{
  g_return_if_fail (MRD_IS_INPUT_INJECTOR (injector));

  CGPoint position = translate_to_global (injector, x, y);

  if (flags & PTR_FLAGS_MOVE) {
    /* MouseMoved (no CGWarpMouseCursorPosition): saves a per-event mach_msg.
     * Side effect: live window-drag (LeftMouseDragged) doesn't engage, so
     * drag-to-edge tiling won't fire — keyboard tilers still work. */
    CGEventRef event = CGEventCreateMouseEvent (NULL, kCGEventMouseMoved,
                                                position, kCGMouseButtonLeft);
    if (event) {
      CGEventPost (kCGHIDEventTap, event);
      CFRelease (event);
    }
    injector->last_mouse_position = position;
  }

  if (flags & PTR_FLAGS_BUTTON1) {
    gboolean down = (flags & PTR_FLAGS_DOWN) != 0;
    post_mouse_button (injector, 0,
                       down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp,
                       position, kCGMouseButtonLeft, down);
  }

  if (flags & PTR_FLAGS_BUTTON2) {
    gboolean down = (flags & PTR_FLAGS_DOWN) != 0;
    post_mouse_button (injector, 1,
                       down ? kCGEventRightMouseDown : kCGEventRightMouseUp,
                       position, kCGMouseButtonRight, down);
  }

  if (flags & PTR_FLAGS_BUTTON3) {
    gboolean down = (flags & PTR_FLAGS_DOWN) != 0;
    post_mouse_button (injector, 2,
                       down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp,
                       position, kCGMouseButtonCenter, down);
  }

  if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) {
    /* MS-RDPBCGR 2.2.8.1.1.3.1.1.3: low 8 bits = magnitude in WHEEL_DELTA
     * (120) units, PTR_FLAGS_WHEEL_NEGATIVE = sign. Pixel units (not lines)
     * keep small trackpad deltas from rounding to zero. */
    enum { WHEEL_DELTA = 120, DEFAULT_PX_PER_CLICK = 30 };

    static gsize scale_init = 0;
    static int pixels_per_click = DEFAULT_PX_PER_CLICK;
    if (g_once_init_enter (&scale_init)) {
      const char *s = g_getenv ("MRD_SCROLL_SCALE");
      if (s && s[0]) {
        char *end = NULL;
        long v = strtol (s, &end, 10);
        if (end && *end == '\0' && v >= 1 && v <= 500) {
          pixels_per_click = (int) v;
          g_message ("MRD_SCROLL_SCALE=%d px/click (override)", pixels_per_click);
        }
      }
      g_once_init_leave (&scale_init, 1);
    }

    /* Decode like FreeRDP client side: value = byte; NEGATIVE → -(256-byte).
     * Naive `-byte` causes ~30× overscroll in the negative direction. */
    gint32 byte = (flags & 0xFF);
    gint32 magnitude = byte;
    if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
      magnitude = -(0x100 - byte);

    /* Log first handful of events per session so we can confirm the
     * client's encoding and the interpreted magnitudes both directions. */
    static int logged_count = 0;
    if (logged_count < 16) {
      g_message ("Wheel event %d: flags=0x%04x byte=0x%02x magnitude=%d%s",
                 logged_count, flags, byte, magnitude,
                 (flags & PTR_FLAGS_HWHEEL) ? " [H]" : "");
      logged_count++;
    }

    /* Pixel delta with symmetric rounding so small magnitudes don't
     * systematically favor one direction. */
    gint32 pixels = (magnitude * pixels_per_click) / WHEEL_DELTA;
    if (pixels == 0 && magnitude != 0)
      pixels = (magnitude > 0) ? 1 : -1;

    gint32 y_delta = 0, x_delta = 0;
    if (flags & PTR_FLAGS_HWHEEL)
      x_delta = pixels;
    else
      y_delta = pixels;

    CGEventRef event = CGEventCreateScrollWheelEvent (
      NULL, kCGScrollEventUnitPixel, 2, y_delta, x_delta);
    if (event) {
      CGEventPost (kCGHIDEventTap, event);
      CFRelease (event);
    }
  }
}

void
mrd_input_injector_handle_extended_mouse (MrdInputInjector *injector,
                                          guint16           flags,
                                          guint16           x,
                                          guint16           y)
{
  g_return_if_fail (MRD_IS_INPUT_INJECTOR (injector));

  CGPoint position = translate_to_global (injector, x, y);

  /* Button 4 (back) */
  if (flags & PTR_XFLAGS_BUTTON1) {
    gboolean down = (flags & PTR_XFLAGS_DOWN) != 0;
    CGEventType type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
    CGEventRef event = CGEventCreateMouseEvent (NULL, type, position, 3);
    if (event) {
      CGEventPost (kCGHIDEventTap, event);
      CFRelease (event);
    }
  }

  /* Button 5 (forward) */
  if (flags & PTR_XFLAGS_BUTTON2) {
    gboolean down = (flags & PTR_XFLAGS_DOWN) != 0;
    CGEventType type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
    CGEventRef event = CGEventCreateMouseEvent (NULL, type, position, 4);
    if (event) {
      CGEventPost (kCGHIDEventTap, event);
      CFRelease (event);
    }
  }
}

void
mrd_input_injector_set_target_display (MrdInputInjector *injector,
                                       uint32_t          display_id)
{
  g_return_if_fail (MRD_IS_INPUT_INJECTOR (injector));
  injector->target_display_id = display_id;
}

void
mrd_input_injector_set_client_size (MrdInputInjector *injector,
                                    uint32_t          width,
                                    uint32_t          height)
{
  g_return_if_fail (MRD_IS_INPUT_INJECTOR (injector));
  injector->client_width = width;
  injector->client_height = height;
}

gboolean
mrd_input_injector_get_client_cursor_position (MrdInputInjector *injector,
                                               guint16          *out_x,
                                               guint16          *out_y)
{
  g_return_val_if_fail (MRD_IS_INPUT_INJECTOR (injector), FALSE);
  g_return_val_if_fail (out_x != NULL && out_y != NULL, FALSE);

  if (injector->target_display_id == 0 ||
      injector->client_width == 0 || injector->client_height == 0)
    return FALSE;

  CGRect bounds = CGDisplayBounds (injector->target_display_id);
  if (CGRectIsNull (bounds) || CGRectIsEmpty (bounds))
    return FALSE;

  CGEventRef ev = CGEventCreate (NULL);
  if (!ev)
    return FALSE;
  CGPoint global = CGEventGetLocation (ev);
  CFRelease (ev);

  CGFloat lx = global.x - bounds.origin.x;
  CGFloat ly = global.y - bounds.origin.y;

  /* Cursor on a different display (host has multi-display + no mirror). */
  if (lx < 0 || ly < 0 || lx >= bounds.size.width || ly >= bounds.size.height)
    return FALSE;

  CGFloat cx = lx * (CGFloat)injector->client_width  / bounds.size.width;
  CGFloat cy = ly * (CGFloat)injector->client_height / bounds.size.height;

  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  if (cx > (CGFloat)(injector->client_width  - 1)) cx = (CGFloat)(injector->client_width  - 1);
  if (cy > (CGFloat)(injector->client_height - 1)) cy = (CGFloat)(injector->client_height - 1);

  *out_x = (guint16) cx;
  *out_y = (guint16) cy;
  return TRUE;
}

void
mrd_input_injector_release_modifiers (MrdInputInjector *injector)
{
  g_return_if_fail (MRD_IS_INPUT_INJECTOR (injector));

  injector->tracked_flags = 0;
  memset (injector->pressed, 0, sizeof (injector->pressed));

  /* Force-release stuck modifiers from client sync/focus events. */
  static const CGKeyCode modifiers[] = {
    kVK_Control, kVK_RightControl,
    kVK_Shift,   kVK_RightShift,
    kVK_Option,  kVK_RightOption,
    kVK_Command, kVK_RightCommand,
    kVK_Function,
  };

  for (size_t i = 0; i < G_N_ELEMENTS (modifiers); i++) {
    CGEventRef event = CGEventCreateKeyboardEvent (NULL, modifiers[i], false);
    if (event) {
      CGEventSetFlags (event, 0);
      CGEventPost (kCGHIDEventTap, event);
      CFRelease (event);
    }
  }

  g_message ("Released all modifier keys (flags cleared)");
}

static void
mrd_input_injector_init (MrdInputInjector *injector)
{
  injector->last_mouse_position = CGPointMake (0, 0);
  injector->pressed_buttons = 0;
  injector->target_display_id = 0;
}

static void
mrd_input_injector_class_init (MrdInputInjectorClass *klass)
{
}
