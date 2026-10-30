/*
 * ADST / ZTEway (BYD BF5325 + AisinoChip ACH512) 2009:7638 SZOEMHost protocol
 *
 * Pure framing, checksum, and SCSI Bulk-Only Transport (BOT) helpers for the
 * USB 2009:7638 Match-On-Chip fingerprint sensor ("ADST Fingerprint",
 * BYD BF5325 160x160 @ 508 DPI capacitive matrix + ACH512 security MCU).
 *
 * SAFETY INVARIANT:
 *   Bootloader/ISP mode-switch, sensor factory calibration, persistent parameter
 *   write, and device password lock opcodes (0x0171, 0x0005, 0x0137, 0x013A,
 *   0x0126) are intentionally omitted and MUST NEVER be defined or transmitted.
 *
 * Copyright (C) 2026 Rubén García Sánchez
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ADST_USB_VID                    0x2009
#define ADST_USB_PID                    0x7638

#define ADST_CBW_LEN                    31
#define ADST_CSW_LEN                    13
#define ADST_PKT_LEN                    24
#define ADST_PKT_BODY_LEN               22
#define ADST_PKT_MAX_DATA               16
#define ADST_BULK_MAX_PKT               64

#define ADST_WAIT_BYTE                  0xAF

#define ADST_CMD_PREFIX                 0xAA55
#define ADST_RCM_PREFIX                 0x55AA

/* Vendor SCSI CDB opcodes over Bulk-Only Transport */
#define ADST_SCSI_OP_VENDOR             0xEF
#define ADST_SCSI_SUB_DEVINFO_REQ       0x01
#define ADST_SCSI_SUB_DEVINFO_STAT      0x02
#define ADST_SCSI_SUB_DEVINFO_DATA      0x03
#define ADST_SCSI_SUB_WRITE_CMD         0x11
#define ADST_SCSI_SUB_READ_RSP          0x12

/* Safe operational SZOEMHost command opcodes */
#define ADST_CMD_VERIFY                 0x0101
#define ADST_CMD_IDENTIFY               0x0102
#define ADST_CMD_ENROLL                 0x0103
#define ADST_CMD_CLEAR_TEMPLATE         0x0105
#define ADST_CMD_CLEAR_ALLTEMPLATE      0x0106
#define ADST_CMD_GET_EMPTY_ID           0x0107
#define ADST_CMD_GET_FW_VERSION         0x0112
#define ADST_CMD_FINGER_DETECT          0x0113
#define ADST_CMD_SLED_CTRL              0x0124
#define ADST_CMD_GET_ENROLL_COUNT       0x0128
#define ADST_CMD_FP_CANCEL              0x0130
#define ADST_CMD_TEST_CONNECTION        0x0150
#define ADST_RCM_INCORRECT_COMMAND      0x0160

/* SZOEMHost return codes (ret field at bytes [6..7]) */
#define ADST_RET_SUCCESS                0x0000
#define ADST_RET_FAIL                   0x0001

/* SZOEMHost error codes (low byte of data1 when ret == ADST_RET_FAIL) */
#define ADST_ERR_VERIFY                 0x11
#define ADST_ERR_IDENTIFY               0x12
#define ADST_ERR_TMPL_EMPTY             0x13
#define ADST_ERR_TMPL_NOT_EMPTY         0x14
#define ADST_ERR_ALL_TMPL_EMPTY         0x15
#define ADST_ERR_EMPTY_ID_NOEXIST       0x16
#define ADST_ERR_INVALID_TMPL_DATA      0x18
#define ADST_ERR_DUPLICATION_ID         0x19
#define ADST_ERR_TOO_FAST               0x20
#define ADST_ERR_BAD_QUALITY            0x21
#define ADST_ERR_SMALL_LINES            0x22
#define ADST_ERR_TIME_OUT               0x23
#define ADST_ERR_NOT_AUTHORIZED         0x24
#define ADST_ERR_GENERALIZE             0x30
#define ADST_ERR_FP_CANCEL              0x41
#define ADST_ERR_INTERNAL               0x50
#define ADST_ERR_MEMORY                 0x51
#define ADST_ERR_EXCEPTION              0x52
#define ADST_ERR_INVALID_TMPL_NO        0x60
#define ADST_ERR_INVALID_PARAM          0x70
#define ADST_ERR_NO_RELEASE             0x71
#define ADST_ERR_INVALID_OP_MODE        0x72
#define ADST_ERR_FP_NOT_DETECTED        0x75

/* SZOEMHost enrollment / capture progress prompts (data1 when ret == 0) */
#define ADST_GD_NEED_FIRST_SWEEP        0xFFF1
#define ADST_GD_NEED_SECOND_SWEEP       0xFFF2
#define ADST_GD_NEED_THIRD_SWEEP        0xFFF3
#define ADST_GD_NEED_RELEASE_FINGER     0xFFF4

#define ADST_ENROLL_STAGES              3
#define ADST_MIN_SLOT_ID                1
#define ADST_MAX_SLOT_ID                5000

typedef struct
{
  uint16_t prefix;
  uint16_t cmd;
  uint16_t data_len;
  uint16_t ret;
  uint16_t data1;
  uint16_t data2;
  uint16_t checksum;
} AdstResponse;

static inline uint16_t
adst_read_le16 (const uint8_t *p)
{
  return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static inline void
adst_write_le16 (uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t) (v & 0xFF);
  p[1] = (uint8_t) ((v >> 8) & 0xFF);
}

static inline uint16_t
adst_checksum22 (const uint8_t pkt[ADST_PKT_LEN])
{
  uint16_t sum = 0;

  for (size_t i = 0; i < ADST_PKT_BODY_LEN; i++)
    sum = (uint16_t) (sum + pkt[i]);

  return sum;
}

/*
 * Builds a 31-byte USB Mass Storage CBW matching the vendor ACH512 BOT parser:
 *   Signature: "USBC" (55 53 42 43)
 *   Tag:       28 2b 18 89
 *   DataLen:   00 00 00 00 (vendor firmware ignores dCBWDataTransferLength)
 *   Flags:     0x80 (IN) or 0x00 (OUT)
 *   LUN:       0x00, CBLength: 0x0A
 *   CDB[0..7]: caller-supplied vendor 0xEF CDB
 */
static inline void
adst_build_cbw (uint8_t       cbw[ADST_CBW_LEN],
                bool          dir_in,
                const uint8_t cdb[8])
{
  memset (cbw, 0, ADST_CBW_LEN);
  cbw[0] = 0x55; /* 'U' */
  cbw[1] = 0x53; /* 'S' */
  cbw[2] = 0x42; /* 'B' */
  cbw[3] = 0x43; /* 'C' */
  cbw[4] = 0x28;
  cbw[5] = 0x2B;
  cbw[6] = 0x18;
  cbw[7] = 0x89;
  cbw[12] = dir_in ? 0x80 : 0x00;
  cbw[13] = 0x00;
  cbw[14] = 0x0A;
  memcpy (cbw + 15, cdb, 8);
}

static inline void
adst_build_cbw_write_cmd (uint8_t cbw[ADST_CBW_LEN])
{
  const uint8_t cdb[8] = {
    ADST_SCSI_OP_VENDOR, ADST_SCSI_SUB_WRITE_CMD, 0, 0, ADST_PKT_LEN, 0, 0, 0
  };

  adst_build_cbw (cbw, false, cdb);
}

static inline void
adst_build_cbw_read_rsp (uint8_t cbw[ADST_CBW_LEN])
{
  const uint8_t cdb[8] = {
    ADST_SCSI_OP_VENDOR, ADST_SCSI_SUB_READ_RSP, 0, 0, 0, 0, 0, 0
  };

  adst_build_cbw (cbw, true, cdb);
}

static inline bool
adst_validate_csw (const uint8_t *buf, size_t len)
{
  if (!buf || len != ADST_CSW_LEN)
    return false;

  /* "USBS" signature and bCSWStatus == 0x00 (Command Passed) */
  return buf[0] == 0x55 &&
         buf[1] == 0x53 &&
         buf[2] == 0x42 &&
         buf[3] == 0x53 &&
         buf[12] == 0x00;
}

static inline bool
adst_build_cmd (uint8_t        pkt[ADST_PKT_LEN],
                uint16_t       cmd,
                const uint8_t *data,
                uint16_t       data_len)
{
  uint16_t sum;

  if (!pkt || data_len > ADST_PKT_MAX_DATA || (data_len > 0 && !data))
    return false;

  memset (pkt, 0, ADST_PKT_LEN);
  adst_write_le16 (pkt + 0, ADST_CMD_PREFIX);
  adst_write_le16 (pkt + 2, cmd);
  adst_write_le16 (pkt + 4, data_len);
  if (data_len > 0)
    memcpy (pkt + 6, data, data_len);

  sum = adst_checksum22 (pkt);
  adst_write_le16 (pkt + 22, sum);
  return true;
}

static inline bool
adst_build_cmd_np (uint8_t pkt[ADST_PKT_LEN], uint16_t cmd)
{
  return adst_build_cmd (pkt, cmd, NULL, 0);
}

static inline bool
adst_build_cmd_u16 (uint8_t pkt[ADST_PKT_LEN], uint16_t cmd, uint16_t arg)
{
  uint8_t payload[2];

  adst_write_le16 (payload, arg);
  return adst_build_cmd (pkt, cmd, payload, 2);
}

static inline bool
adst_is_wait_packet (const uint8_t *buf, size_t len)
{
  if (!buf || len != ADST_PKT_LEN)
    return false;

  for (size_t i = 0; i < ADST_PKT_LEN; i++)
    {
      if (buf[i] != ADST_WAIT_BYTE)
        return false;
    }

  return true;
}

static inline bool
adst_parse_response (const uint8_t *buf, size_t len, AdstResponse *out)
{
  uint16_t expected_sum;
  uint16_t actual_sum;
  uint16_t prefix;

  if (!buf || len != ADST_PKT_LEN || !out)
    return false;

  prefix = adst_read_le16 (buf + 0);
  if (prefix != ADST_RCM_PREFIX)
    return false;

  expected_sum = adst_checksum22 (buf);
  actual_sum = adst_read_le16 (buf + 22);
  if (expected_sum != actual_sum)
    return false;

  out->prefix   = prefix;
  out->cmd      = adst_read_le16 (buf + 2);
  out->data_len = adst_read_le16 (buf + 4);
  out->ret      = adst_read_le16 (buf + 6);
  out->data1    = adst_read_le16 (buf + 8);
  out->data2    = adst_read_le16 (buf + 10);
  out->checksum = actual_sum;
  return true;
}

static inline bool
adst_slot_id_is_valid (uint16_t slot_id)
{
  return slot_id >= ADST_MIN_SLOT_ID && slot_id <= ADST_MAX_SLOT_ID;
}
