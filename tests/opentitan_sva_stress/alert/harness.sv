// Copyright (c) 2026 The yosys-slang contributors
// SPDX-License-Identifier: ISC

module prim_alert_rxtx_sva_harness
  import prim_mubi_pkg::mubi4_t;
(
  input logic clk_i,
  input logic rst_ni,
  input logic ping_err_pi,
  input logic ping_err_ni,
  input logic ack_err_pi,
  input logic ack_err_ni,
  input logic alert_err_pi,
  input logic alert_err_ni,
  input logic alert_test_i,
  input logic alert_req_i,
  input mubi4_t init_trig_i,
  input logic ping_req_i
);
  initial assume(!rst_ni);

  logic alert_ack;
  logic alert_state;
  logic ping_ok;
  logic integ_fail;
  logic alert;

  prim_alert_rxtx_tb u_tb (
    .clk_i,
    .rst_ni,
    .ping_err_pi,
    .ping_err_ni,
    .ack_err_pi,
    .ack_err_ni,
    .alert_err_pi,
    .alert_err_ni,
    .alert_test_i,
    .alert_req_i,
    .init_trig_i,
    .ping_req_i,
    .alert_ack_o(alert_ack),
    .alert_state_o(alert_state),
    .ping_ok_o(ping_ok),
    .integ_fail_o(integ_fail),
    .alert_o(alert)
  );

  prim_alert_rxtx_assert_fpv u_assert (
    .clk_i,
    .rst_ni,
    .ping_err_pi,
    .ping_err_ni,
    .ack_err_pi,
    .ack_err_ni,
    .alert_err_pi,
    .alert_err_ni,
    .alert_test_i,
    .alert_req_i,
    .alert_ack_o(alert_ack),
    .alert_state_o(alert_state),
    .init_trig_i,
    .ping_req_i,
    .ping_ok_o(ping_ok),
    .integ_fail_o(integ_fail),
    .alert_o(alert)
  );
endmodule
