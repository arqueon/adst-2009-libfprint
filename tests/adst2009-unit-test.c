/*
 * Unit tests for ADST / BYD BF5325 (2009:7638) SZOEMHost protocol framing
 *
 * Validates SCSI-BOT CBW/CSW construction & checking, 24-byte SZOEMHost
 * command assembly, checksum verification, 0xAF wait-frame detection,
 * and real hardware response vectors captured from 2009:7638 (BF5325 V1.9 /
 * ACH512 MCU).
 *
 * Copyright (C) 2026 Rubén García Sánchez
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "adst2009-proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
test_cbw_csw (void)
{
  uint8_t cbw[ADST_CBW_LEN];

  adst_build_cbw_write_cmd (cbw);
  assert (memcmp (cbw, "USBC", 4) == 0);
  assert (cbw[4] == 0x28 && cbw[5] == 0x2B && cbw[6] == 0x18 && cbw[7] == 0x89);
  assert (cbw[8] == 0 && cbw[9] == 0 && cbw[10] == 0 && cbw[11] == 0);
  assert (cbw[12] == 0x00);
  assert (cbw[14] == 0x0A);
  assert (cbw[15] == 0xEF && cbw[16] == 0x11 && cbw[19] == 24);

  adst_build_cbw_read_rsp (cbw);
  assert (memcmp (cbw, "USBC", 4) == 0);
  assert (cbw[12] == 0x80);
  assert (cbw[15] == 0xEF && cbw[16] == 0x12 && cbw[19] == 0);

  const uint8_t good_csw[ADST_CSW_LEN] = {
    'U', 'S', 'B', 'S', 0x28, 0x2B, 0x18, 0x89, 0, 0, 0, 0, 0x00
  };
  assert (adst_validate_csw (good_csw, sizeof good_csw));

  uint8_t bad_csw[ADST_CSW_LEN];
  memcpy (bad_csw, good_csw, sizeof bad_csw);
  bad_csw[12] = 0x01; /* Command Failed */
  assert (!adst_validate_csw (bad_csw, sizeof bad_csw));
  assert (!adst_validate_csw (good_csw, 12));
}

static void
test_cmd_framing (void)
{
  uint8_t pkt[ADST_PKT_LEN];

  assert (adst_build_cmd_np (pkt, ADST_CMD_TEST_CONNECTION));
  assert (pkt[0] == 0x55 && pkt[1] == 0xAA);
  assert (pkt[2] == 0x50 && pkt[3] == 0x01);
  assert (pkt[4] == 0x00 && pkt[5] == 0x00);
  /* 0x55 + 0xAA + 0x50 + 0x01 = 0x0150 */
  assert (adst_read_le16 (pkt + 22) == 0x0150);

  assert (adst_build_cmd_u16 (pkt, ADST_CMD_ENROLL, 1));
  assert (pkt[2] == 0x03 && pkt[3] == 0x01);
  assert (pkt[4] == 0x02 && pkt[5] == 0x00);
  assert (pkt[6] == 0x01 && pkt[7] == 0x00);
  /* 0x55 + 0xAA + 0x03 + 0x01 + 0x02 + 0x01 = 0x0106 */
  assert (adst_read_le16 (pkt + 22) == 0x0106);

  uint8_t oversize[17] = { 0 };
  assert (!adst_build_cmd (pkt, ADST_CMD_VERIFY, oversize, 17));
  assert (!adst_build_cmd (pkt, ADST_CMD_VERIFY, NULL, 2));
}

static void
test_wait_packet_detection (void)
{
  uint8_t wait_pkt[ADST_PKT_LEN];
  AdstResponse rsp;

  memset (wait_pkt, ADST_WAIT_BYTE, sizeof wait_pkt);
  assert (adst_is_wait_packet (wait_pkt, sizeof wait_pkt));
  assert (!adst_parse_response (wait_pkt, sizeof wait_pkt, &rsp));

  wait_pkt[23] = 0x00;
  assert (!adst_is_wait_packet (wait_pkt, sizeof wait_pkt));
}

static void
test_live_hardware_vectors (void)
{
  AdstResponse rsp;

  /* Live capture #1: GET_FW_VERSION (0x0112) -> V1.9 (data1 = 0x0901) */
  const uint8_t fw_vec[ADST_PKT_LEN] = {
    0xAA, 0x55, 0x12, 0x01, 0x04, 0x00, 0x00, 0x00,
    0x01, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x01
  };
  assert (!adst_is_wait_packet (fw_vec, sizeof fw_vec));
  assert (adst_parse_response (fw_vec, sizeof fw_vec, &rsp));
  assert (rsp.cmd == ADST_CMD_GET_FW_VERSION);
  assert (rsp.ret == ADST_RET_SUCCESS);
  assert (rsp.data1 == 0x0901);
  assert ((rsp.data1 & 0xFF) == 1 && ((rsp.data1 >> 8) & 0xFF) == 9);

  /* Live capture #2: ENROLL (0x0103) prompt GD_NEED_FIRST_SWEEP (0xFFF1) */
  const uint8_t sweep1_vec[ADST_PKT_LEN] = {
    0xAA, 0x55, 0x03, 0x01, 0x04, 0x00, 0x00, 0x00,
    0xF1, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7, 0x02
  };
  assert (adst_parse_response (sweep1_vec, sizeof sweep1_vec, &rsp));
  assert (rsp.cmd == ADST_CMD_ENROLL);
  assert (rsp.ret == ADST_RET_SUCCESS);
  assert (rsp.data1 == ADST_GD_NEED_FIRST_SWEEP);

  /* Live capture #3: ENROLL interrupted by FP_CANCEL -> ret=1, data1=0x0041 */
  const uint8_t cancel_int_vec[ADST_PKT_LEN] = {
    0xAA, 0x55, 0x03, 0x01, 0x04, 0x00, 0x01, 0x00,
    0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x01
  };
  assert (adst_parse_response (cancel_int_vec, sizeof cancel_int_vec, &rsp));
  assert (rsp.cmd == ADST_CMD_ENROLL);
  assert (rsp.ret == ADST_RET_FAIL);
  assert ((rsp.data1 & 0xFF) == ADST_ERR_FP_CANCEL);

  /* Live capture #4: IDENTIFY on empty chip -> ret=1, data1=0x0015 */
  const uint8_t empty_id_vec[ADST_PKT_LEN] = {
    0xAA, 0x55, 0x02, 0x01, 0x04, 0x00, 0x01, 0x00,
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x01
  };
  assert (adst_parse_response (empty_id_vec, sizeof empty_id_vec, &rsp));
  assert (rsp.cmd == ADST_CMD_IDENTIFY);
  assert (rsp.ret == ADST_RET_FAIL);
  assert ((rsp.data1 & 0xFF) == ADST_ERR_ALL_TMPL_EMPTY);

  /* Checksum corruption must fail parsing */
  uint8_t corrupted[ADST_PKT_LEN];
  memcpy (corrupted, fw_vec, sizeof corrupted);
  corrupted[8] ^= 0x01;
  assert (!adst_parse_response (corrupted, sizeof corrupted, &rsp));
}

static void
test_slot_bounds (void)
{
  assert (!adst_slot_id_is_valid (0));
  assert (adst_slot_id_is_valid (1));
  assert (adst_slot_id_is_valid (5000));
  assert (!adst_slot_id_is_valid (5001));
  assert (!adst_slot_id_is_valid (ADST_GD_NEED_FIRST_SWEEP));
  assert (!adst_slot_id_is_valid (ADST_GD_NEED_RELEASE_FINGER));
}

int
main (void)
{
  test_cbw_csw ();
  test_cmd_framing ();
  test_wait_packet_detection ();
  test_live_hardware_vectors ();
  test_slot_bounds ();

  printf ("adst2009-unit-test: all 5 protocol test suites passed.\n");
  return 0;
}
