/*
 * adst2009-probe — Safe read-only diagnostic tool for ADST 2009:7638
 *
 * Queries the BYD BF5325 / AisinoChip ACH512 Match-On-Chip USB fingerprint
 * reader (2009:7638) using strictly read-only SZOEMHost & vendor 0xEF SCSI
 * commands (GetDeviceInformation, TEST_CONNECTION, GET_FW_VERSION,
 * GET_ENROLL_COUNT, GET_EMPTY_ID). Never modifies flash templates, parameters,
 * passwords, or firmware modes, and never re-attaches usb-storage on exit.
 *
 * Copyright (C) 2026 Rubén García Sánchez
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "adst2009-proto.h"

#include <libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EP_OUT     0x01
#define EP_IN      0x81
#define TIMEOUT_MS 2500

static int
scsi_write (libusb_device_handle *h,
            const uint8_t         cdb[8],
            const uint8_t        *payload,
            int                   plen)
{
  uint8_t cbw[ADST_CBW_LEN];
  uint8_t csw_buf[ADST_BULK_MAX_PKT];
  int xfer = 0;

  adst_build_cbw (cbw, false, cdb);
  if (libusb_bulk_transfer (h, EP_OUT, cbw, ADST_CBW_LEN, &xfer, TIMEOUT_MS) != 0 ||
      xfer != ADST_CBW_LEN)
    return -1;

  if (plen > 0)
    {
      if (libusb_bulk_transfer (h, EP_OUT, (unsigned char *) payload, plen, &xfer, TIMEOUT_MS) != 0 ||
          xfer != plen)
        return -1;
    }

  if (libusb_bulk_transfer (h, EP_IN, csw_buf, sizeof csw_buf, &xfer, TIMEOUT_MS) != 0 ||
      !adst_validate_csw (csw_buf, (size_t) xfer))
    return -1;

  return 0;
}

static int
scsi_read (libusb_device_handle *h,
           const uint8_t         cdb[8],
           uint8_t              *out,
           int                   max_len)
{
  uint8_t cbw[ADST_CBW_LEN];
  uint8_t rx_buf[ADST_BULK_MAX_PKT];
  uint8_t csw_buf[ADST_BULK_MAX_PKT];
  int got = 0;
  int xfer = 0;

  adst_build_cbw (cbw, true, cdb);
  if (libusb_bulk_transfer (h, EP_OUT, cbw, ADST_CBW_LEN, &xfer, TIMEOUT_MS) != 0 ||
      xfer != ADST_CBW_LEN)
    return -1;

  if (libusb_bulk_transfer (h, EP_IN, rx_buf, sizeof rx_buf, &got, TIMEOUT_MS) != 0 ||
      got <= 0)
    return -1;

  if (libusb_bulk_transfer (h, EP_IN, csw_buf, sizeof csw_buf, &xfer, TIMEOUT_MS) != 0 ||
      !adst_validate_csw (csw_buf, (size_t) xfer))
    return -1;

  if (got > max_len)
    got = max_len;
  memcpy (out, rx_buf, (size_t) got);
  return got;
}

static int
cmd_exchange (libusb_device_handle *h, uint16_t cmd, AdstResponse *rsp_out)
{
  uint8_t tx[ADST_PKT_LEN];
  uint8_t rx[ADST_PKT_LEN];
  const uint8_t cdb_w[8] = {
    ADST_SCSI_OP_VENDOR, ADST_SCSI_SUB_WRITE_CMD, 0, 0, ADST_PKT_LEN, 0, 0, 0
  };
  const uint8_t cdb_r[8] = {
    ADST_SCSI_OP_VENDOR, ADST_SCSI_SUB_READ_RSP, 0, 0, 0, 0, 0, 0
  };

  if (!adst_build_cmd_np (tx, cmd))
    return -1;
  if (scsi_write (h, cdb_w, tx, ADST_PKT_LEN) != 0)
    return -1;

  for (int i = 0; i < 40; i++)
    {
      int got = scsi_read (h, cdb_r, rx, sizeof rx);
      if (got < 0)
        return -1;
      if (!adst_is_wait_packet (rx, (size_t) got))
        {
          if (!adst_parse_response (rx, (size_t) got, rsp_out))
            return -1;
          return 0;
        }
      usleep (20000);
    }

  return -1;
}

static void
cancel_and_drain (libusb_device_handle *h)
{
  uint8_t tx[ADST_PKT_LEN];
  uint8_t rx[ADST_PKT_LEN];
  const uint8_t cdb_w[8] = {
    ADST_SCSI_OP_VENDOR, ADST_SCSI_SUB_WRITE_CMD, 0, 0, ADST_PKT_LEN, 0, 0, 0
  };
  const uint8_t cdb_r[8] = {
    ADST_SCSI_OP_VENDOR, ADST_SCSI_SUB_READ_RSP, 0, 0, 0, 0, 0, 0
  };
  bool saw_ack = false;

  adst_build_cmd_np (tx, ADST_CMD_FP_CANCEL);
  if (scsi_write (h, cdb_w, tx, ADST_PKT_LEN) != 0)
    return;

  for (int i = 0; i < 25; i++)
    {
      AdstResponse rsp;
      int got = scsi_read (h, cdb_r, rx, sizeof rx);
      if (got < 0)
        break;
      if (adst_is_wait_packet (rx, (size_t) got))
        {
          if (saw_ack)
            break;
        }
      else if (adst_parse_response (rx, (size_t) got, &rsp) &&
               rsp.cmd == ADST_CMD_FP_CANCEL)
        {
          saw_ack = true;
        }
      usleep (20000);
    }
}

static void
query_device_info (libusb_device_handle *h)
{
  const uint8_t cdb_req[8]  = { ADST_SCSI_OP_VENDOR, ADST_SCSI_SUB_DEVINFO_REQ, 0x04, 0, 0, 0, 0, 0 };
  const uint8_t cdb_stat[8] = { ADST_SCSI_OP_VENDOR, ADST_SCSI_SUB_DEVINFO_STAT, 0, 0, 0, 0, 0, 0 };
  const uint8_t cdb_data[8] = { ADST_SCSI_OP_VENDOR, ADST_SCSI_SUB_DEVINFO_DATA, 0, 0, 0, 0, 0, 0 };
  uint8_t stat[4] = { 0 };
  uint8_t info[33] = { 0 };

  if (scsi_write (h, cdb_req, NULL, 0) != 0)
    return;
  if (scsi_read (h, cdb_stat, stat, sizeof stat) < 4)
    return;

  uint16_t len = adst_read_le16 (stat + 2);
  if (len == 0 || len > 32)
    return;

  if (scsi_read (h, cdb_data, info, len) == (int) len)
    {
      char sensor_str[17] = { 0 };
      char mcu_str[17] = { 0 };
      memcpy (sensor_str, info, 12);
      memcpy (mcu_str, info + 16, 16);
      printf ("  Sensor Info   : %s\n", sensor_str);
      printf ("  Security MCU  : %s\n", mcu_str);
    }
}

int
main (void)
{
  libusb_context *ctx = NULL;
  libusb_device_handle *h = NULL;
  AdstResponse rsp;
  int rc = 1;

  if (libusb_init (&ctx) != 0)
    {
      fprintf (stderr, "adst2009-probe: libusb_init failed\n");
      return 1;
    }

  h = libusb_open_device_with_vid_pid (ctx, ADST_USB_VID, ADST_USB_PID);
  if (!h)
    {
      fprintf (stderr,
               "adst2009-probe: USB device %04x:%04x not found (or permission denied).\n",
               ADST_USB_VID, ADST_USB_PID);
      libusb_exit (ctx);
      return 2;
    }

  /* Detach usb-storage explicitly WITHOUT auto-reattach on close */
  if (libusb_kernel_driver_active (h, 0) == 1)
    libusb_detach_kernel_driver (h, 0);

  if (libusb_claim_interface (h, 0) != 0)
    {
      fprintf (stderr, "adst2009-probe: failed to claim USB interface 0 (is fprintd holding it?)\n");
      libusb_close (h);
      libusb_exit (ctx);
      return 3;
    }

  printf ("ADST / BYD BF5325 (%04x:%04x) Match-On-Chip Diagnostic Probe\n",
          ADST_USB_VID, ADST_USB_PID);

  cancel_and_drain (h);
  query_device_info (h);

  if (cmd_exchange (h, ADST_CMD_TEST_CONNECTION, &rsp) != 0 ||
      rsp.cmd != ADST_CMD_TEST_CONNECTION ||
      rsp.ret != ADST_RET_SUCCESS)
    {
      fprintf (stderr, "  TEST_CONNECTION (0x0150) failed.\n");
      goto out;
    }
  printf ("  Connection    : OK (SZOEMHost 24-byte protocol)\n");

  if (cmd_exchange (h, ADST_CMD_GET_FW_VERSION, &rsp) == 0 &&
      rsp.ret == ADST_RET_SUCCESS)
    {
      printf ("  Firmware Ver  : V%u.%u (0x%04x)\n",
              (unsigned) (rsp.data1 & 0xFF),
              (unsigned) ((rsp.data1 >> 8) & 0xFF),
              (unsigned) rsp.data1);
    }

  if (cmd_exchange (h, ADST_CMD_GET_ENROLL_COUNT, &rsp) == 0 &&
      rsp.ret == ADST_RET_SUCCESS)
    {
      printf ("  Enrolled Slots: %u\n", (unsigned) rsp.data1);
    }

  if (cmd_exchange (h, ADST_CMD_GET_EMPTY_ID, &rsp) == 0)
    {
      if (rsp.ret == ADST_RET_SUCCESS)
        printf ("  Next Free Slot: %u\n", (unsigned) rsp.data1);
      else if ((rsp.data1 & 0xFF) == ADST_ERR_EMPTY_ID_NOEXIST)
        printf ("  Next Free Slot: FULL\n");
    }

  rc = 0;

out:
  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (ctx);
  return rc;
}
