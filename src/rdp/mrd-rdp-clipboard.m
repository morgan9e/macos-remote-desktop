/*
 * Threading: FreeRDP 3.24.2's cliprdr server has no external-pump API.
 * ctx->Start() spawns an internal thread for client callbacks; poll_host
 * runs on the session thread. last_change_count is the only field shared
 * across threads (guarded by self->lock). ctx->Server*() is thread-safe.
 */

#import <AppKit/AppKit.h>

#include "mrd-rdp-clipboard.h"

#include <gio/gio.h>
#include <string.h>

#include <freerdp/channels/cliprdr.h>
#include <winpr/string.h>
#include <winpr/wtypes.h>

#define CF_UNICODETEXT 13   /* Windows UTF-16LE text format id */

struct _MrdRdpClipboard
{
  GObject parent;

  MrdSessionRdp *session;  /* not owned */
  HANDLE vcm;              /* not owned */

  CliprdrServerContext *ctx;
  gboolean started;

  GMutex lock;   /* guards last_change_count */
  NSInteger last_change_count;

  /* Cliprdr-thread only; cleared in stop after ctx->Stop joins. */
  gboolean awaiting_client_data;
};

G_DEFINE_TYPE (MrdRdpClipboard, mrd_rdp_clipboard, G_TYPE_OBJECT)

static NSString *
pasteboard_string (NSInteger *out_change_count)
{
  NSPasteboard *pb = [NSPasteboard generalPasteboard];
  if (out_change_count)
    *out_change_count = [pb changeCount];
  NSString *s = [pb stringForType:NSPasteboardTypeString];
  return s;
}

static void
set_pasteboard_string (MrdRdpClipboard *self, NSString *s)
{
  NSPasteboard *pb = [NSPasteboard generalPasteboard];
  [pb clearContents];
  if (s)
    [pb setString:s forType:NSPasteboardTypeString];
  /* Record AFTER set so next poll doesn't echo this back to the client. */
  g_mutex_lock (&self->lock);
  self->last_change_count = [pb changeCount];
  g_mutex_unlock (&self->lock);
}

static gboolean
send_format_list_text (MrdRdpClipboard *self)
{
  CLIPRDR_FORMAT format = { 0 };
  format.formatId = CF_UNICODETEXT;
  format.formatName = NULL;  /* registered id → name empty */

  CLIPRDR_FORMAT_LIST format_list = { 0 };
  format_list.common.msgType = CB_FORMAT_LIST;
  format_list.numFormats = 1;
  format_list.formats = &format;

  UINT rc = self->ctx->ServerFormatList (self->ctx, &format_list);
  if (rc != CHANNEL_RC_OK)
    {
      g_warning ("[CLIPRDR] ServerFormatList failed: 0x%08x", rc);
      return FALSE;
    }
  return TRUE;
}

static void
send_empty_format_list (MrdRdpClipboard *self)
{
  CLIPRDR_FORMAT_LIST format_list = { 0 };
  format_list.common.msgType = CB_FORMAT_LIST;
  format_list.numFormats = 0;
  format_list.formats = NULL;
  (void) self->ctx->ServerFormatList (self->ctx, &format_list);
}

/* Callbacks below run on FreeRDP's internal cliprdr thread. */

static UINT
on_client_capabilities (CliprdrServerContext       *ctx,
                        const CLIPRDR_CAPABILITIES *capabilities)
{
  (void) capabilities;
  g_message ("[CLIPRDR] Client capabilities negotiated: long-format-names=%s",
             ctx->useLongFormatNames ? "yes" : "no");
  return CHANNEL_RC_OK;
}

static UINT
on_temp_directory (CliprdrServerContext         *ctx,
                   const CLIPRDR_TEMP_DIRECTORY *temp_dir)
{
  (void) ctx;
  (void) temp_dir;
  return CHANNEL_RC_OK;
}

static UINT
on_client_format_list (CliprdrServerContext      *ctx,
                       const CLIPRDR_FORMAT_LIST *format_list)
{
  MrdRdpClipboard *self = ctx->custom;

  CLIPRDR_FORMAT_LIST_RESPONSE response = { 0 };
  response.common.msgType = CB_FORMAT_LIST_RESPONSE;
  response.common.msgFlags = CB_RESPONSE_OK;
  UINT rc = ctx->ServerFormatListResponse (ctx, &response);
  if (rc != CHANNEL_RC_OK)
    g_warning ("[CLIPRDR] ServerFormatListResponse failed: 0x%08x", rc);

  gboolean has_text = FALSE;
  for (UINT32 i = 0; i < format_list->numFormats; i++)
    {
      if (format_list->formats[i].formatId == CF_UNICODETEXT)
        {
          has_text = TRUE;
          break;
        }
    }

  if (!has_text || self->awaiting_client_data)
    return CHANNEL_RC_OK;

  CLIPRDR_FORMAT_DATA_REQUEST req = { 0 };
  req.common.msgType = CB_FORMAT_DATA_REQUEST;
  req.common.dataLen = 4;
  req.requestedFormatId = CF_UNICODETEXT;

  self->awaiting_client_data = TRUE;
  rc = ctx->ServerFormatDataRequest (ctx, &req);
  if (rc != CHANNEL_RC_OK)
    {
      self->awaiting_client_data = FALSE;
      g_warning ("[CLIPRDR] ServerFormatDataRequest failed: 0x%08x", rc);
    }
  return CHANNEL_RC_OK;
}

static UINT
on_client_format_list_response (CliprdrServerContext                *ctx,
                                const CLIPRDR_FORMAT_LIST_RESPONSE  *response)
{
  (void) ctx;
  (void) response;
  return CHANNEL_RC_OK;
}

static UINT
on_client_format_data_request (CliprdrServerContext              *ctx,
                               const CLIPRDR_FORMAT_DATA_REQUEST *req)
{
  CLIPRDR_FORMAT_DATA_RESPONSE response = { 0 };
  response.common.msgType = CB_FORMAT_DATA_RESPONSE;

  BYTE *utf16_bytes = NULL;
  size_t utf16_byte_len = 0;

  if (req->requestedFormatId == CF_UNICODETEXT)
    {
      @autoreleasepool
        {
          NSString *s = pasteboard_string (NULL);
          if (s)
            {
              const char *utf8 = [s UTF8String];
              size_t wlen = 0;
              WCHAR *wbuf = ConvertUtf8NToWCharAlloc (utf8, strlen (utf8), &wlen);
              if (wbuf)
                {
                  /* RDP spec requires trailing \0\0 in the payload. */
                  utf16_byte_len = (wlen + 1) * sizeof (WCHAR);
                  utf16_bytes = g_malloc (utf16_byte_len);
                  memcpy (utf16_bytes, wbuf, wlen * sizeof (WCHAR));
                  utf16_bytes[wlen * sizeof (WCHAR)]     = 0;
                  utf16_bytes[wlen * sizeof (WCHAR) + 1] = 0;
                  free (wbuf);
                }
            }
        }
    }

  if (utf16_bytes)
    {
      response.common.msgFlags = CB_RESPONSE_OK;
      response.common.dataLen = (UINT32) utf16_byte_len;
      response.requestedFormatData = utf16_bytes;
    }
  else
    {
      response.common.msgFlags = CB_RESPONSE_FAIL;
      response.common.dataLen = 0;
      response.requestedFormatData = NULL;
    }

  UINT rc = ctx->ServerFormatDataResponse (ctx, &response);
  if (rc != CHANNEL_RC_OK)
    g_warning ("[CLIPRDR] ServerFormatDataResponse failed: 0x%08x", rc);

  g_free (utf16_bytes);
  return CHANNEL_RC_OK;
}

static UINT
on_client_format_data_response (CliprdrServerContext               *ctx,
                                const CLIPRDR_FORMAT_DATA_RESPONSE *response)
{
  MrdRdpClipboard *self = ctx->custom;
  self->awaiting_client_data = FALSE;

  if ((response->common.msgFlags & CB_RESPONSE_OK) == 0 ||
      response->common.dataLen < sizeof (WCHAR) ||
      response->requestedFormatData == NULL)
    return CHANNEL_RC_OK;

  const WCHAR *wbuf = (const WCHAR *) response->requestedFormatData;
  size_t wlen = response->common.dataLen / sizeof (WCHAR);

  while (wlen > 0 && wbuf[wlen - 1] == 0)
    wlen--;

  if (wlen == 0)
    {
      @autoreleasepool
        {
          set_pasteboard_string (self, @"");
        }
      return CHANNEL_RC_OK;
    }

  size_t utf8_len = 0;
  char *utf8 = ConvertWCharNToUtf8Alloc (wbuf, wlen, &utf8_len);
  if (!utf8)
    return CHANNEL_RC_OK;

  @autoreleasepool
    {
      NSString *s = [[NSString alloc] initWithBytes:utf8
                                             length:utf8_len
                                           encoding:NSUTF8StringEncoding];
      if (s)
        set_pasteboard_string (self, s);
    }
  free (utf8);
  return CHANNEL_RC_OK;
}

MrdRdpClipboard *
mrd_rdp_clipboard_new (MrdSessionRdp *session, HANDLE vcm)
{
  g_return_val_if_fail (vcm != NULL, NULL);

  MrdRdpClipboard *self = g_object_new (MRD_TYPE_RDP_CLIPBOARD, NULL);
  self->session = session;
  self->vcm = vcm;

  self->ctx = cliprdr_server_context_new (vcm);
  if (!self->ctx)
    {
      g_warning ("[CLIPRDR] cliprdr_server_context_new failed");
      g_object_unref (self);
      return NULL;
    }

  self->ctx->useLongFormatNames = TRUE;
  self->ctx->streamFileClipEnabled = FALSE;
  self->ctx->fileClipNoFilePaths = FALSE;
  self->ctx->canLockClipData = FALSE;
  self->ctx->hasHugeFileSupport = FALSE;

  self->ctx->ClientCapabilities = on_client_capabilities;
  self->ctx->TempDirectory = on_temp_directory;
  self->ctx->ClientFormatList = on_client_format_list;
  self->ctx->ClientFormatListResponse = on_client_format_list_response;
  self->ctx->ClientFormatDataRequest = on_client_format_data_request;
  self->ctx->ClientFormatDataResponse = on_client_format_data_response;
  self->ctx->custom = self;

  /* Prime so first poll doesn't push pre-connect pasteboard contents. */
  @autoreleasepool
    {
      self->last_change_count = [[NSPasteboard generalPasteboard] changeCount];
    }

  return self;
}

gboolean
mrd_rdp_clipboard_start (MrdRdpClipboard *self)
{
  g_return_val_if_fail (MRD_IS_RDP_CLIPBOARD (self), FALSE);
  if (self->started)
    return TRUE;

  UINT rc = self->ctx->Start (self->ctx);
  if (rc != CHANNEL_RC_OK)
    {
      g_warning ("[CLIPRDR] Start failed: 0x%08x", rc);
      return FALSE;
    }
  self->started = TRUE;
  g_message ("[CLIPRDR] Channel started");
  return TRUE;
}

void
mrd_rdp_clipboard_stop (MrdRdpClipboard *self)
{
  g_return_if_fail (MRD_IS_RDP_CLIPBOARD (self));
  if (!self->started)
    return;
  (void) self->ctx->Stop (self->ctx);
  self->started = FALSE;
  self->awaiting_client_data = FALSE;
}

void
mrd_rdp_clipboard_poll_host (MrdRdpClipboard *self)
{
  g_return_if_fail (MRD_IS_RDP_CLIPBOARD (self));
  if (!self->started)
    return;

  @autoreleasepool
    {
      NSInteger change_count = 0;
      NSString *s = pasteboard_string (&change_count);

      g_mutex_lock (&self->lock);
      if (change_count == self->last_change_count)
        {
          g_mutex_unlock (&self->lock);
          return;
        }
      self->last_change_count = change_count;
      g_mutex_unlock (&self->lock);

      if (s && [s length] > 0)
        send_format_list_text (self);
      else
        send_empty_format_list (self);
    }
}

static void
mrd_rdp_clipboard_finalize (GObject *object)
{
  MrdRdpClipboard *self = MRD_RDP_CLIPBOARD (object);

  if (self->started)
    mrd_rdp_clipboard_stop (self);
  if (self->ctx)
    {
      cliprdr_server_context_free (self->ctx);
      self->ctx = NULL;
    }
  g_mutex_clear (&self->lock);

  G_OBJECT_CLASS (mrd_rdp_clipboard_parent_class)->finalize (object);
}

static void
mrd_rdp_clipboard_init (MrdRdpClipboard *self)
{
  g_mutex_init (&self->lock);
}

static void
mrd_rdp_clipboard_class_init (MrdRdpClipboardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = mrd_rdp_clipboard_finalize;
}
