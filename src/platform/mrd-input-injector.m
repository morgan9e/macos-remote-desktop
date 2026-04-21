#include "mrd-input-injector.h"
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>

#define KBD_FLAGS_EXTENDED   0x0100
#define KBD_FLAGS_DOWN       0x4000
#define KBD_FLAGS_RELEASE    0x8000

#define PTR_FLAGS_MOVE       0x0800
#define PTR_FLAGS_DOWN       0x8000
#define PTR_FLAGS_BUTTON1    0x1000  
#define PTR_FLAGS_BUTTON2    0x2000  
#define PTR_FLAGS_BUTTON3    0x4000  
#define PTR_FLAGS_WHEEL      0x0200
#define PTR_FLAGS_WHEEL_NEGATIVE 0x0100

#define PTR_XFLAGS_DOWN      0x8000
#define PTR_XFLAGS_BUTTON1   0x0001  
#define PTR_XFLAGS_BUTTON2   0x0002  
struct _MrdInputInjector
{
  GObject parent;
  CGPoint last_mouse_position;  
  guint32 pressed_buttons;
  uint32_t target_display_id;   
  
  uint32_t client_width;
  uint32_t client_height;
  
  int64_t last_click_time_us[3];   
  CGPoint last_click_pos[3];
  int64_t click_count[3];
  
  CGEventFlags tracked_flags;
  
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
  gx = MAX (bounds.origin.x, MIN (gx, bounds.origin.x + bounds.size.width  - 1));
  gy = MAX (bounds.origin.y, MIN (gy, bounds.origin.y + bounds.size.height - 1));
  return CGPointMake (gx, gy);
}

static CGKeyCode
scancode_to_keycode (guint8 scancode, gboolean extended)
{
  
  static const CGKeyCode basic_map[128] = {
     0xFF,
     kVK_Escape,
     kVK_ANSI_1,
     kVK_ANSI_2,
     kVK_ANSI_3,
     kVK_ANSI_4,
     kVK_ANSI_5,
     kVK_ANSI_6,
     kVK_ANSI_7,
     kVK_ANSI_8,
     kVK_ANSI_9,
     kVK_ANSI_0,
     kVK_ANSI_Minus,
     kVK_ANSI_Equal,
     kVK_Delete,            
     kVK_Tab,
     kVK_ANSI_Q,
     kVK_ANSI_W,
     kVK_ANSI_E,
     kVK_ANSI_R,
     kVK_ANSI_T,
     kVK_ANSI_Y,
     kVK_ANSI_U,
     kVK_ANSI_I,
     kVK_ANSI_O,
     kVK_ANSI_P,
     kVK_ANSI_LeftBracket,
     kVK_ANSI_RightBracket,
     kVK_Return,
     kVK_Control,           
     kVK_ANSI_A,
     kVK_ANSI_S,
     kVK_ANSI_D,
     kVK_ANSI_F,
     kVK_ANSI_G,
     kVK_ANSI_H,
     kVK_ANSI_J,
     kVK_ANSI_K,
     kVK_ANSI_L,
     kVK_ANSI_Semicolon,
     kVK_ANSI_Quote,
     kVK_ANSI_Grave,        
     kVK_Shift,             
     kVK_ANSI_Backslash,
     kVK_ANSI_Z,
     kVK_ANSI_X,
     kVK_ANSI_C,
     kVK_ANSI_V,
     kVK_ANSI_B,
     kVK_ANSI_N,
     kVK_ANSI_M,
     kVK_ANSI_Comma,
     kVK_ANSI_Period,
     kVK_ANSI_Slash,
     kVK_RightShift,
     kVK_ANSI_KeypadMultiply,
     kVK_Command,           
     kVK_Space,
     kVK_CapsLock,
     kVK_F1,
     kVK_F2,
     kVK_F3,
     kVK_F4,
     kVK_F5,
     kVK_F6,
     kVK_F7,
     kVK_F8,
     kVK_F9,
     kVK_F10,
     kVK_ANSI_KeypadClear,  
     kVK_F14,               
     kVK_ANSI_Keypad7,
     kVK_ANSI_Keypad8,
     kVK_ANSI_Keypad9,
     kVK_ANSI_KeypadMinus,
     kVK_ANSI_Keypad4,
     kVK_ANSI_Keypad5,
     kVK_ANSI_Keypad6,
     kVK_ANSI_KeypadPlus,
     kVK_ANSI_Keypad1,
     kVK_ANSI_Keypad2,
     kVK_ANSI_Keypad3,
     kVK_ANSI_Keypad0,
     kVK_ANSI_KeypadDecimal,
     0xFF,                  
     0xFF,
     kVK_ISO_Section,       
     kVK_F11,
     kVK_F12,
     kVK_ANSI_KeypadEquals, 
     0xFF,
     0xFF,
     0xFF,
     0xFF,
     0xFF,
     0xFF,
     0xFF,
     0xFF,
     0xFF,
     0xFF,
     kVK_F13,
     kVK_F14,
     kVK_F15,
     kVK_F16,
     kVK_F17,
     kVK_F18,
     kVK_F19,
     kVK_F20,
     0xFF,                  
     0xFF,                  
     0xFF,                  
     0xFF,                  
     0xFF,                  
     0xFF,
     0xFF,
     kVK_JIS_Underscore,    
     0xFF,
     0xFF,
     0xFF,
     0xFF,
     0xFF,
     kVK_JIS_KeypadComma,   
     0xFF,
     kVK_JIS_Yen,           
     0xFF,
     0xFF,
     0xFF,
     0xFF,                  
  };
  
  if (extended) {
    switch (scancode) {
      
      case 0x1C: return kVK_ANSI_KeypadEnter;
      case 0x1D: return kVK_RightControl;
      case 0x35: return kVK_ANSI_KeypadDivide;
      case 0x38: return kVK_RightCommand;  
      case 0x47: return kVK_Home;
      case 0x48: return kVK_UpArrow;
      case 0x49: return kVK_PageUp;
      case 0x4B: return kVK_LeftArrow;
      case 0x4D: return kVK_RightArrow;
      case 0x4F: return kVK_End;
      case 0x50: return kVK_DownArrow;
      case 0x51: return kVK_PageDown;
      case 0x52: return kVK_Help;          
      case 0x53: return kVK_ForwardDelete;
      
      case 0x5B: return kVK_Option;        
      case 0x5C: return kVK_RightOption;   
      
      case 0x5D: return 0x6E;              
      
      case 0x5E: return 0xFF;              
      case 0x5F: return 0xFF;              
      case 0x63: return 0xFF;              
      
      case 0x10: return 0xFF;              
      case 0x19: return 0xFF;              
      case 0x20: return 0xFF;              
      case 0x21: return 0xFF;              
      case 0x22: return 0xFF;              
      case 0x24: return 0xFF;              
      case 0x2E: return 0xFF;              
      case 0x30: return 0xFF;              
      case 0x32: return 0xFF;              
      
      case 0x37: return kVK_F13;           
      
      case 0x46: return kVK_F15;           
      default: return 0xFF;
    }
  }
  if (scancode < 128)
    return basic_map[scancode];
  return 0xFF;
}

static void
post_mouse_button (MrdInputInjector *injector,
                   int               button_idx,   
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
  g_message ("Key: scancode=0x%02X ext=%d keycode=%u %s  [flags: Ctrl=%d Shift=%d Opt=%d Cmd=%d Fn=%d]",
             scancode, extended, keycode, key_down ? "DOWN" : "UP",
             (injector->tracked_flags & kCGEventFlagMaskControl)       ? 1 : 0,
             (injector->tracked_flags & kCGEventFlagMaskShift)         ? 1 : 0,
             (injector->tracked_flags & kCGEventFlagMaskAlternate)     ? 1 : 0,
             (injector->tracked_flags & kCGEventFlagMaskCommand)       ? 1 : 0,
             (injector->tracked_flags & kCGEventFlagMaskSecondaryFn)   ? 1 : 0);
  CGEventRef event = CGEventCreateKeyboardEvent (NULL, keycode, key_down);
  if (event) {
    
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
  if (flags & PTR_FLAGS_WHEEL) {
    gint32 delta = (flags & 0xFF);
    if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
      delta = -delta;
    CGEventRef event = CGEventCreateScrollWheelEvent (NULL, kCGScrollEventUnitLine, 1, delta);
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
  
  if (flags & PTR_XFLAGS_BUTTON1) {
    gboolean down = (flags & PTR_XFLAGS_DOWN) != 0;
    CGEventType type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
    CGEventRef event = CGEventCreateMouseEvent (NULL, type, position, 3);
    if (event) {
      CGEventPost (kCGHIDEventTap, event);
      CFRelease (event);
    }
  }
  
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
void
mrd_input_injector_release_modifiers (MrdInputInjector *injector)
{
  g_return_if_fail (MRD_IS_INPUT_INJECTOR (injector));
  
  injector->tracked_flags = 0;
  memset (injector->pressed, 0, sizeof (injector->pressed));
  
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