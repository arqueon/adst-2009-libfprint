/*
 * ADST / ZTEway (BYD BF5325 + AisinoChip ACH512) 2009:7638 MOC driver for libfprint-2
 *
 * Native Match-On-Chip (FpDevice) driver for the USB 2009:7638 fingerprint reader
 * ("iStorage / ADST Fingerprint", BYD BF5325 160x160 @ 508 DPI capacitive matrix
 * paired with an AisinoChip ACH512 biometric security MCU running the 24-byte
 * SZOEMHost command protocol over vendor 0xEF SCSI Bulk-Only Transport).
 *
 * SAFETY INVARIANT:
 *   Bootloader/ISP mode-switch, factory sensor calibration, persistent parameter
 *   write, and device password lock opcodes (0x0171, 0x0005, 0x0137, 0x013A,
 *   0x0126) are NEVER defined or transmitted.
 *
 * Copyright (C) 2026 Rubén García Sánchez
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "adst2009"
#include "fpi-log.h"

#include "drivers_api.h"
#include "adst2009-proto.h"

#include <string.h>

#define ADST_EP_OUT             (0x01 | FPI_USB_ENDPOINT_OUT)
#define ADST_EP_IN              (0x01 | FPI_USB_ENDPOINT_IN)

#define ADST_USB_TIMEOUT_MS     2500
#define ADST_CANCEL_TIMEOUT_MS  800

#define ADST_SYNC_DELAY_MS      20
#define ADST_SYNC_MAX_POLLS     40

#define ADST_WAIT_DELAY_MS      75
#define ADST_WAIT_MAX_POLLS     1200 /* ~90 s safety ceiling per touch wait */

#define ADST_CANCEL_DELAY_MS    20
#define ADST_CANCEL_MAX_POLLS   25

enum init_states {
  INIT_CANCEL_SEND,
  INIT_CANCEL_DELAY,
  INIT_CANCEL_POLL,
  INIT_CANCEL_CHECK,
  INIT_TEST_SEND,
  INIT_TEST_DELAY,
  INIT_TEST_POLL,
  INIT_TEST_CHECK,
  INIT_FW_SEND,
  INIT_FW_DELAY,
  INIT_FW_POLL,
  INIT_FW_CHECK,
  INIT_COUNT_SEND,
  INIT_COUNT_DELAY,
  INIT_COUNT_POLL,
  INIT_COUNT_CHECK,
  INIT_NUM_STATES,
};

enum enroll_states {
  ENROLL_EMPTY_SEND,
  ENROLL_EMPTY_DELAY,
  ENROLL_EMPTY_POLL,
  ENROLL_EMPTY_CHECK,
  ENROLL_START_SEND,
  ENROLL_WAIT_DELAY,
  ENROLL_WAIT_POLL,
  ENROLL_WAIT_PARSE,
  ENROLL_CANCEL_SEND,
  ENROLL_CANCEL_DELAY,
  ENROLL_CANCEL_POLL,
  ENROLL_CANCEL_CHECK,
  ENROLL_CANCEL_DONE,
  ENROLL_NUM_STATES,
};

enum match_states {
  MATCH_START_SEND,
  MATCH_WAIT_DELAY,
  MATCH_WAIT_POLL,
  MATCH_WAIT_PARSE,
  MATCH_CANCEL_SEND,
  MATCH_CANCEL_DELAY,
  MATCH_CANCEL_POLL,
  MATCH_CANCEL_CHECK,
  MATCH_CANCEL_DONE,
  MATCH_NUM_STATES,
};

enum del_states {
  DEL_SEND,
  DEL_DELAY,
  DEL_POLL,
  DEL_CHECK,
  DEL_NUM_STATES,
};

struct _FpiDeviceAdst2009
{
  FpDevice     parent;

  FpiSsm      *ssm;
  gboolean     is_probe;
  gboolean     cmd_active;

  guint16      fw_version;
  guint16      enrolled_count;

  guint16      target_slot;
  gint         completed_stages;

  gint         polls_left;
  gint         wait_polls;
  gboolean     saw_cancel_ack;
  gboolean     best_effort;
  GError      *cancel_error;

  guint8       cbw_buf[ADST_CBW_LEN];
  guint8       cmd_buf[ADST_PKT_LEN];
  guint8       rsp_buf[ADST_BULK_MAX_PKT];
  gsize        rsp_len;
  gboolean     rsp_is_wait;
  gboolean     rsp_valid;
  AdstResponse rsp;
};

G_DECLARE_FINAL_TYPE (FpiDeviceAdst2009, fpi_device_adst2009,
                      FPI, DEVICE_ADST2009, FpDevice);
G_DEFINE_TYPE (FpiDeviceAdst2009, fpi_device_adst2009, FP_TYPE_DEVICE);

/* ---- Utility helpers ----------------------------------------------------- */

static void
adst_secure_zero (void *ptr, gsize len)
{
  volatile guint8 *p = (volatile guint8 *) ptr;

  while (len--)
    *p++ = 0;
}

static gboolean
adst_is_emulated (void)
{
  return g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0;
}

static GError *
adst_retry_error (guint8 err_code)
{
  switch (err_code)
    {
    case ADST_ERR_TOO_FAST:
    case ADST_ERR_FP_NOT_DETECTED:
      return fpi_device_retry_new (FP_DEVICE_RETRY_TOO_SHORT);

    case ADST_ERR_SMALL_LINES:
      return fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);

    case ADST_ERR_NO_RELEASE:
      return fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER);

    case ADST_ERR_BAD_QUALITY:
    default:
      return fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);
    }
}

static void
adst_populate_print (FpPrint *print, guint16 slot_id)
{
  GVariant *fpi_data;
  g_autofree gchar *desc = NULL;

  fpi_data = g_variant_new ("(q)", slot_id);
  desc = g_strdup_printf ("adst2009-slot-%u", (guint) slot_id);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", fpi_data, NULL);
  fp_print_set_description (print, desc);
}

static gboolean
adst_extract_slot_id (FpPrint *print, guint16 *slot_out)
{
  g_autoptr(GVariant) data = NULL;
  guint16 slot_id = 0;

  if (!print || !slot_out)
    return FALSE;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("(q)")))
    return FALSE;

  g_variant_get (data, "(q)", &slot_id);
  if (!adst_slot_id_is_valid (slot_id))
    return FALSE;

  *slot_out = slot_id;
  return TRUE;
}

/* ---- Atomic 3-URB SCSI-BOT transaction engine (CBW -> Data -> CSW) ------- */

static guint
adst_xfer_timeout (FpiDeviceAdst2009 *self)
{
  return self->best_effort ? ADST_CANCEL_TIMEOUT_MS : ADST_USB_TIMEOUT_MS;
}

static void
adst_handle_xfer_error (FpiDeviceAdst2009 *self, FpiSsm *ssm, GError *error)
{
  if (self->best_effort)
    {
      g_clear_error (&error);
      self->rsp_is_wait = TRUE;
      self->rsp_valid = FALSE;
      fpi_ssm_next_state (ssm);
      return;
    }

  fpi_ssm_mark_failed (ssm, error);
}

static void
adst_write_csw_cb (FpiUsbTransfer *transfer,
                   FpDevice       *dev,
                   gpointer        user_data,
                   GError         *error)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);

  if (error)
    {
      adst_handle_xfer_error (self, transfer->ssm, error);
      return;
    }

  if (!adst_validate_csw (transfer->buffer, (size_t) transfer->actual_length))
    {
      adst_handle_xfer_error (self,
                              transfer->ssm,
                              fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                        "Invalid SCSI CSW after EF 11 command write (len=%d)",
                                                        (gint) transfer->actual_length));
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
adst_write_data_cb (FpiUsbTransfer *transfer,
                    FpDevice       *dev,
                    gpointer        user_data,
                    GError         *error)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  FpiUsbTransfer *t;

  if (error)
    {
      adst_handle_xfer_error (self, transfer->ssm, error);
      return;
    }

  t = fpi_usb_transfer_new (dev);
  t->ssm = transfer->ssm;
  fpi_usb_transfer_fill_bulk (t, ADST_EP_IN, ADST_BULK_MAX_PKT);
  fpi_usb_transfer_submit (t, adst_xfer_timeout (self), NULL, adst_write_csw_cb, NULL);
}

static void
adst_write_cbw_cb (FpiUsbTransfer *transfer,
                   FpDevice       *dev,
                   gpointer        user_data,
                   GError         *error)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  FpiUsbTransfer *t;

  if (error)
    {
      adst_handle_xfer_error (self, transfer->ssm, error);
      return;
    }

  t = fpi_usb_transfer_new (dev);
  t->ssm = transfer->ssm;
  t->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (t, ADST_EP_OUT, self->cmd_buf, ADST_PKT_LEN, NULL);
  fpi_usb_transfer_submit (t, adst_xfer_timeout (self), NULL, adst_write_data_cb, NULL);
}

static void
adst_submit_cmd (FpiDeviceAdst2009 *self,
                 FpiSsm            *ssm,
                 guint16            cmd,
                 const guint8      *data,
                 guint16            data_len,
                 gboolean           best_effort)
{
  FpiUsbTransfer *t;

  self->best_effort = best_effort;

  if (!adst_build_cmd (self->cmd_buf, cmd, data, data_len))
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  if (adst_is_emulated ())
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  adst_build_cbw_write_cmd (self->cbw_buf);

  t = fpi_usb_transfer_new (FP_DEVICE (self));
  t->ssm = ssm;
  t->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (t, ADST_EP_OUT, self->cbw_buf, ADST_CBW_LEN, NULL);
  fpi_usb_transfer_submit (t, adst_xfer_timeout (self), NULL, adst_write_cbw_cb, NULL);
}

static void
adst_submit_cmd_np (FpiDeviceAdst2009 *self,
                    FpiSsm            *ssm,
                    guint16            cmd,
                    gboolean           best_effort)
{
  adst_submit_cmd (self, ssm, cmd, NULL, 0, best_effort);
}

static void
adst_submit_cmd_u16 (FpiDeviceAdst2009 *self,
                     FpiSsm            *ssm,
                     guint16            cmd,
                     guint16            arg,
                     gboolean           best_effort)
{
  guint8 payload[2];

  adst_write_le16 (payload, arg);
  adst_submit_cmd (self, ssm, cmd, payload, 2, best_effort);
}

static void
adst_read_csw_cb (FpiUsbTransfer *transfer,
                  FpDevice       *dev,
                  gpointer        user_data,
                  GError         *error)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);

  if (error)
    {
      adst_handle_xfer_error (self, transfer->ssm, error);
      return;
    }

  if (!adst_validate_csw (transfer->buffer, (size_t) transfer->actual_length))
    {
      adst_handle_xfer_error (self,
                              transfer->ssm,
                              fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                        "Invalid SCSI CSW after EF 12 response read (len=%d)",
                                                        (gint) transfer->actual_length));
      return;
    }

  self->rsp_is_wait = adst_is_wait_packet (self->rsp_buf, self->rsp_len);
  self->rsp_valid = !self->rsp_is_wait &&
                    adst_parse_response (self->rsp_buf, self->rsp_len, &self->rsp);

  fpi_ssm_next_state (transfer->ssm);
}

static void
adst_read_data_cb (FpiUsbTransfer *transfer,
                   FpDevice       *dev,
                   gpointer        user_data,
                   GError         *error)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  FpiUsbTransfer *t;

  if (error)
    {
      adst_handle_xfer_error (self, transfer->ssm, error);
      return;
    }

  if (transfer->actual_length <= 0 || transfer->actual_length > ADST_BULK_MAX_PKT)
    {
      adst_handle_xfer_error (self,
                              transfer->ssm,
                              fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                        "Unexpected EF 12 payload size %d",
                                                        (gint) transfer->actual_length));
      return;
    }

  memcpy (self->rsp_buf, transfer->buffer, (size_t) transfer->actual_length);
  self->rsp_len = (gsize) transfer->actual_length;

  t = fpi_usb_transfer_new (dev);
  t->ssm = transfer->ssm;
  fpi_usb_transfer_fill_bulk (t, ADST_EP_IN, ADST_BULK_MAX_PKT);
  fpi_usb_transfer_submit (t, adst_xfer_timeout (self), NULL, adst_read_csw_cb, NULL);
}

static void
adst_read_cbw_cb (FpiUsbTransfer *transfer,
                  FpDevice       *dev,
                  gpointer        user_data,
                  GError         *error)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  FpiUsbTransfer *t;

  if (error)
    {
      adst_handle_xfer_error (self, transfer->ssm, error);
      return;
    }

  t = fpi_usb_transfer_new (dev);
  t->ssm = transfer->ssm;
  fpi_usb_transfer_fill_bulk (t, ADST_EP_IN, ADST_BULK_MAX_PKT);
  fpi_usb_transfer_submit (t, adst_xfer_timeout (self), NULL, adst_read_data_cb, NULL);
}

static void
adst_submit_poll_rsp (FpiDeviceAdst2009 *self,
                      FpiSsm            *ssm,
                      gboolean           best_effort)
{
  FpiUsbTransfer *t;

  self->best_effort = best_effort;
  self->rsp_len = 0;
  self->rsp_is_wait = FALSE;
  self->rsp_valid = FALSE;
  memset (&self->rsp, 0, sizeof self->rsp);

  if (adst_is_emulated ())
    {
      self->rsp_is_wait = TRUE;
      fpi_ssm_next_state (ssm);
      return;
    }

  adst_build_cbw_read_rsp (self->cbw_buf);

  t = fpi_usb_transfer_new (FP_DEVICE (self));
  t->ssm = ssm;
  t->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (t, ADST_EP_OUT, self->cbw_buf, ADST_CBW_LEN, NULL);
  fpi_usb_transfer_submit (t, adst_xfer_timeout (self), NULL, adst_read_cbw_cb, NULL);
}

/*
 * Helper for cancel-drain checks: after sending ADST_CMD_FP_CANCEL (0x0130),
 * the MCU emits any interrupted command's 0x0041 (ERR_FP_CANCEL) packet first,
 * followed by the 0x0130 ACK packet, followed by 0xAF..AF idle wait frames.
 * Draining until saw_cancel_ack && rsp_is_wait guarantees an empty FIFO.
 */
static gboolean
adst_cancel_drain_step (FpiDeviceAdst2009 *self, FpiSsm *ssm, gint delay_state)
{
  if (self->rsp_valid && self->rsp.cmd == ADST_CMD_FP_CANCEL)
    self->saw_cancel_ack = TRUE;

  if (self->saw_cancel_ack && self->rsp_is_wait)
    return TRUE;

  if (--self->polls_left <= 0)
    return TRUE;

  fpi_ssm_jump_to_state (ssm, delay_state);
  return FALSE;
}

/* ---- Probe & Open initialization SSM ------------------------------------- */

static void
adst_init_ssm (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case INIT_CANCEL_SEND:
      if (adst_is_emulated ())
        {
          fpi_ssm_mark_completed (ssm);
          break;
        }
      self->saw_cancel_ack = FALSE;
      self->polls_left = ADST_CANCEL_MAX_POLLS;
      adst_submit_cmd_np (self, ssm, ADST_CMD_FP_CANCEL, FALSE);
      break;

    case INIT_CANCEL_DELAY:
      fpi_ssm_next_state_delayed (ssm, ADST_CANCEL_DELAY_MS);
      break;

    case INIT_CANCEL_POLL:
      adst_submit_poll_rsp (self, ssm, FALSE);
      break;

    case INIT_CANCEL_CHECK:
      if (adst_cancel_drain_step (self, ssm, INIT_CANCEL_DELAY))
        fpi_ssm_next_state (ssm);
      break;

    case INIT_TEST_SEND:
      self->polls_left = ADST_SYNC_MAX_POLLS;
      adst_submit_cmd_np (self, ssm, ADST_CMD_TEST_CONNECTION, FALSE);
      break;

    case INIT_TEST_DELAY:
      fpi_ssm_next_state_delayed (ssm, ADST_SYNC_DELAY_MS);
      break;

    case INIT_TEST_POLL:
      adst_submit_poll_rsp (self, ssm, FALSE);
      break;

    case INIT_TEST_CHECK:
      if (self->rsp_is_wait)
        {
          if (--self->polls_left > 0)
            fpi_ssm_jump_to_state (ssm, INIT_TEST_DELAY);
          else
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "Timed out waiting for TEST_CONNECTION"));
          break;
        }
      if (!self->rsp_valid ||
          self->rsp.cmd != ADST_CMD_TEST_CONNECTION ||
          self->rsp.ret != ADST_RET_SUCCESS)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "TEST_CONNECTION failed"));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case INIT_FW_SEND:
      self->polls_left = ADST_SYNC_MAX_POLLS;
      adst_submit_cmd_np (self, ssm, ADST_CMD_GET_FW_VERSION, FALSE);
      break;

    case INIT_FW_DELAY:
      fpi_ssm_next_state_delayed (ssm, ADST_SYNC_DELAY_MS);
      break;

    case INIT_FW_POLL:
      adst_submit_poll_rsp (self, ssm, FALSE);
      break;

    case INIT_FW_CHECK:
      if (self->rsp_is_wait)
        {
          if (--self->polls_left > 0)
            fpi_ssm_jump_to_state (ssm, INIT_FW_DELAY);
          else
            fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      if (!self->rsp_valid ||
          self->rsp.cmd != ADST_CMD_GET_FW_VERSION ||
          self->rsp.ret != ADST_RET_SUCCESS)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      self->fw_version = self->rsp.data1;
      fp_dbg ("ADST 2009:7638 BYD BF5325 / ACH512 firmware V%u.%u",
              (guint) (self->fw_version & 0xFF),
              (guint) ((self->fw_version >> 8) & 0xFF));
      fpi_ssm_next_state (ssm);
      break;

    case INIT_COUNT_SEND:
      self->polls_left = ADST_SYNC_MAX_POLLS;
      adst_submit_cmd_np (self, ssm, ADST_CMD_GET_ENROLL_COUNT, FALSE);
      break;

    case INIT_COUNT_DELAY:
      fpi_ssm_next_state_delayed (ssm, ADST_SYNC_DELAY_MS);
      break;

    case INIT_COUNT_POLL:
      adst_submit_poll_rsp (self, ssm, FALSE);
      break;

    case INIT_COUNT_CHECK:
      if (self->rsp_is_wait)
        {
          if (--self->polls_left > 0)
            fpi_ssm_jump_to_state (ssm, INIT_COUNT_DELAY);
          else
            fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      if (!self->rsp_valid ||
          self->rsp.cmd != ADST_CMD_GET_ENROLL_COUNT ||
          self->rsp.ret != ADST_RET_SUCCESS)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      self->enrolled_count = self->rsp.data1;
      fp_dbg ("ADST 2009:7638 on-chip enrolled templates: %u",
              (guint) self->enrolled_count);
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
adst_init_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);

  self->ssm = NULL;
  self->cmd_active = FALSE;

  if (self->is_probe)
    {
      /* Release WITHOUT G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER so
       * usb-storage is never re-attached to the fake CD-ROM interface. */
      g_usb_device_release_interface (usb_dev, 0, G_USB_DEVICE_CLAIM_INTERFACE_NONE, NULL);
      g_usb_device_close (usb_dev, NULL);

      if (error)
        {
          fpi_device_probe_complete (dev, NULL, NULL, error);
          return;
        }

      if (adst_is_emulated ())
        fpi_device_probe_complete (dev, "emulated-adst2009", NULL, NULL);
      else
        fpi_device_probe_complete (dev, NULL, NULL, NULL);
      return;
    }

  if (error)
    {
      g_usb_device_release_interface (usb_dev, 0, G_USB_DEVICE_CLAIM_INTERFACE_NONE, NULL);
      g_usb_device_close (usb_dev, NULL);
    }

  fpi_device_open_complete (dev, error);
}

static void
adst_probe (FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);
  GError *error = NULL;

  if (!g_usb_device_open (usb_dev, &error))
    {
      fpi_device_probe_complete (dev, NULL, NULL, error);
      return;
    }

  /* Detach usb-storage on claim if still bound; never re-bind on release. */
  if (!g_usb_device_claim_interface (usb_dev,
                                     0,
                                     G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
                                     &error))
    {
      g_usb_device_close (usb_dev, NULL);
      fpi_device_probe_complete (dev, NULL, NULL, error);
      return;
    }

  self->is_probe = TRUE;
  self->cmd_active = FALSE;
  self->ssm = fpi_ssm_new (dev, adst_init_ssm, INIT_NUM_STATES);
  fpi_ssm_start (self->ssm, adst_init_complete);
}

static void
adst_open (FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);
  GError *error = NULL;

  /* FpDevice core already called g_usb_device_open() before klass->open(). */
  if (!g_usb_device_claim_interface (usb_dev,
                                     0,
                                     G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
                                     &error))
    {
      g_usb_device_close (usb_dev, NULL);
      fpi_device_open_complete (dev, error);
      return;
    }

  self->is_probe = FALSE;
  self->cmd_active = FALSE;
  self->ssm = fpi_ssm_new (dev, adst_init_ssm, INIT_NUM_STATES);
  fpi_ssm_start (self->ssm, adst_init_complete);
}

static void
adst_close (FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);
  GError *error = NULL;

  self->cmd_active = FALSE;
  adst_secure_zero (self->cmd_buf, sizeof self->cmd_buf);
  adst_secure_zero (self->rsp_buf, sizeof self->rsp_buf);

  /* FpDevice core calls g_usb_device_close() inside fpi_device_close_complete(). */
  g_usb_device_release_interface (usb_dev, 0, G_USB_DEVICE_CLAIM_INTERFACE_NONE, &error);
  fpi_device_close_complete (dev, error);
}

/* ---- 3-stage Match-On-Chip Enrollment (0x0107 -> 0x0103) ----------------- */

static void
adst_enroll_ssm (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  GCancellable *cancellable = fpi_device_get_cancellable (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ENROLL_EMPTY_SEND:
      if (g_cancellable_is_cancelled (cancellable))
        {
          fpi_ssm_mark_failed (ssm,
                               g_error_new_literal (G_IO_ERROR,
                                                    G_IO_ERROR_CANCELLED,
                                                    "Enrollment cancelled"));
          break;
        }
      self->polls_left = ADST_SYNC_MAX_POLLS;
      adst_submit_cmd_np (self, ssm, ADST_CMD_GET_EMPTY_ID, FALSE);
      break;

    case ENROLL_EMPTY_DELAY:
      fpi_ssm_next_state_delayed (ssm, ADST_SYNC_DELAY_MS);
      break;

    case ENROLL_EMPTY_POLL:
      adst_submit_poll_rsp (self, ssm, FALSE);
      break;

    case ENROLL_EMPTY_CHECK:
      if (self->rsp_is_wait)
        {
          if (--self->polls_left > 0)
            fpi_ssm_jump_to_state (ssm, ENROLL_EMPTY_DELAY);
          else
            fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      if (!self->rsp_valid || self->rsp.cmd != ADST_CMD_GET_EMPTY_ID)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      if (self->rsp.ret != ADST_RET_SUCCESS)
        {
          guint8 err_code = (guint8) (self->rsp.data1 & 0xFF);
          if (err_code == ADST_ERR_EMPTY_ID_NOEXIST)
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_FULL,
                                                           "Sensor template storage is full"));
          else
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                           "GET_EMPTY_ID failed (0x%02x)",
                                                           err_code));
          break;
        }
      if (!adst_slot_id_is_valid (self->rsp.data1))
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Invalid slot ID %u from sensor",
                                                         (guint) self->rsp.data1));
          break;
        }
      self->target_slot = self->rsp.data1;
      self->completed_stages = 0;
      fp_dbg ("Enrolling into ADST on-chip slot %u", (guint) self->target_slot);
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_START_SEND:
      if (g_cancellable_is_cancelled (cancellable))
        {
          fpi_ssm_mark_failed (ssm,
                               g_error_new_literal (G_IO_ERROR,
                                                    G_IO_ERROR_CANCELLED,
                                                    "Enrollment cancelled"));
          break;
        }
      fpi_device_report_finger_status_changes (dev,
                                               FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_PRESENT);
      self->cmd_active = TRUE;
      self->wait_polls = 0;
      adst_submit_cmd_u16 (self, ssm, ADST_CMD_ENROLL, self->target_slot, FALSE);
      break;

    case ENROLL_WAIT_DELAY:
      if (g_cancellable_is_cancelled (cancellable))
        {
          fpi_ssm_jump_to_state (ssm, ENROLL_CANCEL_SEND);
          break;
        }
      fpi_ssm_next_state_delayed (ssm, ADST_WAIT_DELAY_MS);
      break;

    case ENROLL_WAIT_POLL:
      if (g_cancellable_is_cancelled (cancellable))
        {
          fpi_ssm_jump_to_state (ssm, ENROLL_CANCEL_SEND);
          break;
        }
      adst_submit_poll_rsp (self, ssm, FALSE);
      break;

    case ENROLL_WAIT_PARSE:
      if (self->rsp_is_wait)
        {
          if (g_cancellable_is_cancelled (cancellable))
            {
              fpi_ssm_jump_to_state (ssm, ENROLL_CANCEL_SEND);
              break;
            }
          if (++self->wait_polls > ADST_WAIT_MAX_POLLS)
            {
              g_clear_error (&self->cancel_error);
              self->cancel_error =
                fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                          "Timed out waiting for finger during enrollment");
              fpi_ssm_jump_to_state (ssm, ENROLL_CANCEL_SEND);
              break;
            }
          fpi_ssm_jump_to_state (ssm, ENROLL_WAIT_DELAY);
          break;
        }

      self->wait_polls = 0;

      if (!self->rsp_valid || self->rsp.cmd != ADST_CMD_ENROLL)
        {
          g_clear_error (&self->cancel_error);
          self->cancel_error =
            fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                      "Unexpected packet while enrolling");
          fpi_ssm_jump_to_state (ssm, ENROLL_CANCEL_SEND);
          break;
        }

      if (self->rsp.ret == ADST_RET_SUCCESS)
        {
          switch (self->rsp.data1)
            {
            case ADST_GD_NEED_FIRST_SWEEP:
              fpi_device_report_finger_status_changes (dev,
                                                       FP_FINGER_STATUS_NEEDED,
                                                       FP_FINGER_STATUS_PRESENT);
              fpi_ssm_jump_to_state (ssm, ENROLL_WAIT_DELAY);
              break;

            case ADST_GD_NEED_RELEASE_FINGER:
              fpi_device_report_finger_status_changes (dev,
                                                       FP_FINGER_STATUS_PRESENT,
                                                       FP_FINGER_STATUS_NONE);
              fpi_ssm_jump_to_state (ssm, ENROLL_WAIT_DELAY);
              break;

            case ADST_GD_NEED_SECOND_SWEEP:
              if (self->completed_stages < 1)
                {
                  self->completed_stages = 1;
                  fpi_device_enroll_progress (dev, 1, NULL, NULL);
                }
              fpi_device_report_finger_status_changes (dev,
                                                       FP_FINGER_STATUS_NEEDED,
                                                       FP_FINGER_STATUS_PRESENT);
              fpi_ssm_jump_to_state (ssm, ENROLL_WAIT_DELAY);
              break;

            case ADST_GD_NEED_THIRD_SWEEP:
              while (self->completed_stages < 2)
                {
                  self->completed_stages++;
                  fpi_device_enroll_progress (dev, self->completed_stages, NULL, NULL);
                }
              fpi_device_report_finger_status_changes (dev,
                                                       FP_FINGER_STATUS_NEEDED,
                                                       FP_FINGER_STATUS_PRESENT);
              fpi_ssm_jump_to_state (ssm, ENROLL_WAIT_DELAY);
              break;

            default:
              if (adst_slot_id_is_valid (self->rsp.data1))
                {
                  FpPrint *print = NULL;

                  self->cmd_active = FALSE;
                  self->enrolled_count++;
                  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

                  fpi_device_get_enroll_data (dev, &print);
                  adst_populate_print (print, self->rsp.data1);

                  while (self->completed_stages < ADST_ENROLL_STAGES - 1)
                    {
                      self->completed_stages++;
                      fpi_device_enroll_progress (dev, self->completed_stages, NULL, NULL);
                    }
                  self->completed_stages = ADST_ENROLL_STAGES;
                  fpi_device_enroll_progress (dev, ADST_ENROLL_STAGES, NULL, NULL);

                  fp_dbg ("Enrollment saved in ADST slot %u", (guint) self->rsp.data1);
                  fpi_ssm_mark_completed (ssm);
                }
              else
                {
                  g_clear_error (&self->cancel_error);
                  self->cancel_error =
                    fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                              "Unexpected enroll data1=0x%04x",
                                              (guint) self->rsp.data1);
                  fpi_ssm_jump_to_state (ssm, ENROLL_CANCEL_SEND);
                }
              break;
            }
          break;
        }
      else
        {
          guint8 err_code = (guint8) (self->rsp.data1 & 0xFF);

          /* On ERR_BAD_QUALITY (0x21), SZOEMHost stays inside CMD_ENROLL and
           * awaits the next finger press over continued EF 12 polling. */
          if (err_code == ADST_ERR_BAD_QUALITY)
            {
              fpi_device_enroll_progress (dev,
                                          self->completed_stages,
                                          NULL,
                                          adst_retry_error (err_code));
              fpi_ssm_jump_to_state (ssm, ENROLL_WAIT_DELAY);
              break;
            }

          /* All other ret != 0 codes terminate CMD_ENROLL on the MCU. */
          self->cmd_active = FALSE;
          fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

          if (err_code == ADST_ERR_TIME_OUT &&
              self->completed_stages == 0 &&
              !g_cancellable_is_cancelled (cancellable))
            {
              fpi_ssm_jump_to_state (ssm, ENROLL_START_SEND);
              break;
            }

          if ((err_code == ADST_ERR_TOO_FAST ||
               err_code == ADST_ERR_SMALL_LINES ||
               err_code == ADST_ERR_NO_RELEASE ||
               err_code == ADST_ERR_FP_NOT_DETECTED) &&
              self->completed_stages == 0 &&
              !g_cancellable_is_cancelled (cancellable))
            {
              fpi_device_enroll_progress (dev,
                                          0,
                                          NULL,
                                          adst_retry_error (err_code));
              fpi_ssm_jump_to_state (ssm, ENROLL_START_SEND);
              break;
            }

          if (err_code == ADST_ERR_DUPLICATION_ID)
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_DUPLICATE,
                                                             "Finger already enrolled in sensor slot %u",
                                                             (guint) self->rsp.data2));
              break;
            }

          if (err_code == ADST_ERR_EMPTY_ID_NOEXIST)
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_FULL,
                                                             "Sensor template storage is full"));
              break;
            }

          if (err_code == ADST_ERR_FP_CANCEL)
            {
              fpi_ssm_mark_failed (ssm,
                                   g_error_new_literal (G_IO_ERROR,
                                                        G_IO_ERROR_CANCELLED,
                                                        "Enrollment cancelled"));
              break;
            }

          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                         "Sensor enrollment failed (code 0x%02x)",
                                                         err_code));
          break;
        }

    case ENROLL_CANCEL_SEND:
      self->cmd_active = FALSE;
      self->saw_cancel_ack = FALSE;
      self->polls_left = ADST_CANCEL_MAX_POLLS;
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
      adst_submit_cmd_np (self, ssm, ADST_CMD_FP_CANCEL, TRUE);
      break;

    case ENROLL_CANCEL_DELAY:
      fpi_ssm_next_state_delayed (ssm, ADST_CANCEL_DELAY_MS);
      break;

    case ENROLL_CANCEL_POLL:
      adst_submit_poll_rsp (self, ssm, TRUE);
      break;

    case ENROLL_CANCEL_CHECK:
      if (adst_cancel_drain_step (self, ssm, ENROLL_CANCEL_DELAY))
        fpi_ssm_next_state (ssm);
      break;

    case ENROLL_CANCEL_DONE:
      fpi_ssm_mark_failed (ssm,
                           self->cancel_error
                           ? g_steal_pointer (&self->cancel_error)
                           : g_error_new_literal (G_IO_ERROR,
                                                  G_IO_ERROR_CANCELLED,
                                                  "Enrollment cancelled"));
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
adst_enroll_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  FpPrint *print = NULL;

  self->ssm = NULL;
  self->cmd_active = FALSE;
  g_clear_error (&self->cancel_error);

  if (error)
    {
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  fpi_device_get_enroll_data (dev, &print);
  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

static void
adst_enroll (FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);

  g_clear_error (&self->cancel_error);
  self->ssm = fpi_ssm_new (dev, adst_enroll_ssm, ENROLL_NUM_STATES);
  fpi_ssm_start (self->ssm, adst_enroll_complete);
}

/* ---- Verify (0x0101 1:1) & Identify (0x0102 1:N) ------------------------- */

static void
adst_match_ssm (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);
  GCancellable *cancellable = fpi_device_get_cancellable (dev);
  guint16 expected_cmd = (action == FPI_DEVICE_ACTION_VERIFY)
                         ? ADST_CMD_VERIFY
                         : ADST_CMD_IDENTIFY;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case MATCH_START_SEND:
      if (g_cancellable_is_cancelled (cancellable))
        {
          fpi_ssm_mark_failed (ssm,
                               g_error_new_literal (G_IO_ERROR,
                                                    G_IO_ERROR_CANCELLED,
                                                    "Verification cancelled"));
          break;
        }

      if (action == FPI_DEVICE_ACTION_VERIFY)
        {
          FpPrint *target = NULL;
          guint16 slot_id = 0;

          fpi_device_get_verify_data (dev, &target);
          if (!adst_extract_slot_id (target, &slot_id))
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                             "Enrolled print lacks a valid ADST slot ID"));
              break;
            }
          self->target_slot = slot_id;
        }

      fpi_device_report_finger_status_changes (dev,
                                               FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_PRESENT);
      self->cmd_active = TRUE;
      self->wait_polls = 0;

      if (action == FPI_DEVICE_ACTION_VERIFY)
        adst_submit_cmd_u16 (self, ssm, ADST_CMD_VERIFY, self->target_slot, FALSE);
      else
        adst_submit_cmd_np (self, ssm, ADST_CMD_IDENTIFY, FALSE);
      break;

    case MATCH_WAIT_DELAY:
      if (g_cancellable_is_cancelled (cancellable))
        {
          fpi_ssm_jump_to_state (ssm, MATCH_CANCEL_SEND);
          break;
        }
      fpi_ssm_next_state_delayed (ssm, ADST_WAIT_DELAY_MS);
      break;

    case MATCH_WAIT_POLL:
      if (g_cancellable_is_cancelled (cancellable))
        {
          fpi_ssm_jump_to_state (ssm, MATCH_CANCEL_SEND);
          break;
        }
      adst_submit_poll_rsp (self, ssm, FALSE);
      break;

    case MATCH_WAIT_PARSE:
      if (self->rsp_is_wait)
        {
          if (g_cancellable_is_cancelled (cancellable))
            {
              fpi_ssm_jump_to_state (ssm, MATCH_CANCEL_SEND);
              break;
            }
          if (++self->wait_polls > ADST_WAIT_MAX_POLLS)
            {
              g_clear_error (&self->cancel_error);
              self->cancel_error =
                fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                          "Timed out waiting for finger during verification");
              fpi_ssm_jump_to_state (ssm, MATCH_CANCEL_SEND);
              break;
            }
          fpi_ssm_jump_to_state (ssm, MATCH_WAIT_DELAY);
          break;
        }

      self->wait_polls = 0;

      if (!self->rsp_valid || self->rsp.cmd != expected_cmd)
        {
          g_clear_error (&self->cancel_error);
          self->cancel_error =
            fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                      "Unexpected response during verification");
          fpi_ssm_jump_to_state (ssm, MATCH_CANCEL_SEND);
          break;
        }

      if (self->rsp.ret == ADST_RET_SUCCESS)
        {
          if (self->rsp.data1 == ADST_GD_NEED_FIRST_SWEEP)
            {
              fpi_device_report_finger_status_changes (dev,
                                                       FP_FINGER_STATUS_NEEDED,
                                                       FP_FINGER_STATUS_PRESENT);
              fpi_ssm_jump_to_state (ssm, MATCH_WAIT_DELAY);
              break;
            }

          if (self->rsp.data1 == ADST_GD_NEED_RELEASE_FINGER)
            {
              fpi_device_report_finger_status_changes (dev,
                                                       FP_FINGER_STATUS_PRESENT,
                                                       FP_FINGER_STATUS_NONE);
              fpi_ssm_jump_to_state (ssm, MATCH_WAIT_DELAY);
              break;
            }

          self->cmd_active = FALSE;
          fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

          if (!adst_slot_id_is_valid (self->rsp.data1))
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "Invalid matched slot %u",
                                                             (guint) self->rsp.data1));
              break;
            }

          {
            FpPrint *scan = fp_print_new (dev);
            adst_populate_print (scan, self->rsp.data1);

            if (action == FPI_DEVICE_ACTION_VERIFY)
              {
                FpPrint *target = NULL;
                gboolean ok;

                fpi_device_get_verify_data (dev, &target);
                ok = fp_print_equal (target, scan);
                fpi_device_verify_report (dev,
                                          ok ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                          scan,
                                          NULL);
              }
            else
              {
                GPtrArray *gallery = NULL;
                FpPrint *matched = NULL;

                fpi_device_get_identify_data (dev, &gallery);
                if (gallery)
                  {
                    for (guint i = 0; i < gallery->len; i++)
                      {
                        FpPrint *cand = g_ptr_array_index (gallery, i);
                        if (fp_print_equal (cand, scan))
                          {
                            matched = cand;
                            break;
                          }
                      }
                  }
                fpi_device_identify_report (dev, matched, scan, NULL);
              }

            fpi_ssm_mark_completed (ssm);
            break;
          }
        }
      else
        {
          guint8 err_code = (guint8) (self->rsp.data1 & 0xFF);

          self->cmd_active = FALSE;

          /* MCU internal wait timeout without finger contact -> re-arm silently */
          if (err_code == ADST_ERR_TIME_OUT && !g_cancellable_is_cancelled (cancellable))
            {
              fpi_ssm_jump_to_state (ssm, MATCH_START_SEND);
              break;
            }

          fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

          if (err_code == ADST_ERR_FP_CANCEL)
            {
              fpi_ssm_mark_failed (ssm,
                                   g_error_new_literal (G_IO_ERROR,
                                                        G_IO_ERROR_CANCELLED,
                                                        "Verification cancelled"));
              break;
            }

          /* Sensor returned immediately without waiting because on-chip slot/DB is empty */
          if (err_code == ADST_ERR_TMPL_EMPTY || err_code == ADST_ERR_ALL_TMPL_EMPTY)
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_NOT_FOUND,
                                                             "Fingerprint template not found in sensor storage"));
              break;
            }

          if (err_code == ADST_ERR_VERIFY || err_code == ADST_ERR_IDENTIFY)
            {
              if (action == FPI_DEVICE_ACTION_VERIFY)
                fpi_device_verify_report (dev, FPI_MATCH_FAIL, NULL, NULL);
              else
                fpi_device_identify_report (dev, NULL, NULL, NULL);

              fpi_ssm_mark_completed (ssm);
              break;
            }

          if (err_code == ADST_ERR_BAD_QUALITY ||
              err_code == ADST_ERR_TOO_FAST ||
              err_code == ADST_ERR_SMALL_LINES ||
              err_code == ADST_ERR_NO_RELEASE ||
              err_code == ADST_ERR_FP_NOT_DETECTED)
            {
              GError *retry = adst_retry_error (err_code);

              if (action == FPI_DEVICE_ACTION_VERIFY)
                fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, retry);
              else
                fpi_device_identify_report (dev, NULL, NULL, retry);

              fpi_ssm_mark_completed (ssm);
              break;
            }

          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                         "Sensor match failed (code 0x%02x)",
                                                         err_code));
          break;
        }

    case MATCH_CANCEL_SEND:
      self->cmd_active = FALSE;
      self->saw_cancel_ack = FALSE;
      self->polls_left = ADST_CANCEL_MAX_POLLS;
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
      adst_submit_cmd_np (self, ssm, ADST_CMD_FP_CANCEL, TRUE);
      break;

    case MATCH_CANCEL_DELAY:
      fpi_ssm_next_state_delayed (ssm, ADST_CANCEL_DELAY_MS);
      break;

    case MATCH_CANCEL_POLL:
      adst_submit_poll_rsp (self, ssm, TRUE);
      break;

    case MATCH_CANCEL_CHECK:
      if (adst_cancel_drain_step (self, ssm, MATCH_CANCEL_DELAY))
        fpi_ssm_next_state (ssm);
      break;

    case MATCH_CANCEL_DONE:
      fpi_ssm_mark_failed (ssm,
                           self->cancel_error
                           ? g_steal_pointer (&self->cancel_error)
                           : g_error_new_literal (G_IO_ERROR,
                                                  G_IO_ERROR_CANCELLED,
                                                  "Verification cancelled"));
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
adst_match_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);

  self->ssm = NULL;
  self->cmd_active = FALSE;
  g_clear_error (&self->cancel_error);

  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
    fpi_device_verify_complete (dev, error);
  else
    fpi_device_identify_complete (dev, error);
}

static void
adst_verify_identify (FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);

  g_clear_error (&self->cancel_error);
  self->ssm = fpi_ssm_new (dev, adst_match_ssm, MATCH_NUM_STATES);
  fpi_ssm_start (self->ssm, adst_match_complete);
}

/* ---- Delete single print (0x0105) & Clear all storage (0x0106) ----------- */

static void
adst_del_ssm (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  gboolean is_clear =
    (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_CLEAR_STORAGE);
  guint16 expected_cmd = is_clear
                         ? ADST_CMD_CLEAR_ALLTEMPLATE
                         : ADST_CMD_CLEAR_TEMPLATE;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DEL_SEND:
      self->polls_left = ADST_SYNC_MAX_POLLS;
      if (is_clear)
        adst_submit_cmd_np (self, ssm, ADST_CMD_CLEAR_ALLTEMPLATE, FALSE);
      else
        adst_submit_cmd_u16 (self, ssm, ADST_CMD_CLEAR_TEMPLATE, self->target_slot, FALSE);
      break;

    case DEL_DELAY:
      fpi_ssm_next_state_delayed (ssm, ADST_SYNC_DELAY_MS);
      break;

    case DEL_POLL:
      adst_submit_poll_rsp (self, ssm, FALSE);
      break;

    case DEL_CHECK:
      if (self->rsp_is_wait)
        {
          if (--self->polls_left > 0)
            fpi_ssm_jump_to_state (ssm, DEL_DELAY);
          else
            fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      if (!self->rsp_valid || self->rsp.cmd != expected_cmd)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      if (self->rsp.ret != ADST_RET_SUCCESS)
        {
          guint8 err_code = (guint8) (self->rsp.data1 & 0xFF);
          if (err_code != ADST_ERR_TMPL_EMPTY &&
              err_code != ADST_ERR_ALL_TMPL_EMPTY)
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                             "Sensor template deletion failed (0x%02x)",
                                                             err_code));
              break;
            }
        }
      else
        {
          if (is_clear)
            self->enrolled_count = 0;
          else if (self->enrolled_count > 0)
            self->enrolled_count--;
        }
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
adst_del_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);

  self->ssm = NULL;
  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_CLEAR_STORAGE)
    fpi_device_clear_storage_complete (dev, error);
  else
    fpi_device_delete_complete (dev, error);
}

static void
adst_delete (FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);
  FpPrint *print = NULL;
  guint16 slot_id = 0;

  fpi_device_get_delete_data (dev, &print);
  if (!adst_extract_slot_id (print, &slot_id))
    {
      fp_warn ("Print lacks a valid ADST slot ID; skipping on-chip delete");
      fpi_device_delete_complete (dev, NULL);
      return;
    }

  self->target_slot = slot_id;
  self->ssm = fpi_ssm_new (dev, adst_del_ssm, DEL_NUM_STATES);
  fpi_ssm_start (self->ssm, adst_del_complete);
}

static void
adst_clear_storage (FpDevice *dev)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (dev);

  self->target_slot = 0;
  self->ssm = fpi_ssm_new (dev, adst_del_ssm, DEL_NUM_STATES);
  fpi_ssm_start (self->ssm, adst_del_complete);
}

/* ---- Object lifecycle & driver class registration ------------------------ */

static void
fpi_device_adst2009_finalize (GObject *object)
{
  FpiDeviceAdst2009 *self = FPI_DEVICE_ADST2009 (object);

  adst_secure_zero (self->cmd_buf, sizeof self->cmd_buf);
  adst_secure_zero (self->rsp_buf, sizeof self->rsp_buf);
  g_clear_error (&self->cancel_error);

  G_OBJECT_CLASS (fpi_device_adst2009_parent_class)->finalize (object);
}

static const FpIdEntry id_table[] = {
  { .vid = ADST_USB_VID, .pid = ADST_USB_PID },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_adst2009_init (FpiDeviceAdst2009 *self)
{
}

static void
fpi_device_adst2009_class_init (FpiDeviceAdst2009Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  object_class->finalize = fpi_device_adst2009_finalize;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "ADST / BYD BF5325 2009:7638 MOC Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = ADST_ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->probe = adst_probe;
  dev_class->open = adst_open;
  dev_class->close = adst_close;
  dev_class->enroll = adst_enroll;
  dev_class->verify = adst_verify_identify;
  dev_class->identify = adst_verify_identify;
  dev_class->delete = adst_delete;
  dev_class->clear_storage = adst_clear_storage;

  fpi_device_class_auto_initialize_features (dev_class);
  dev_class->features |= FP_DEVICE_FEATURE_DUPLICATES_CHECK;
}
