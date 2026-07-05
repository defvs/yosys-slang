// Copyright (c) 2026 The yosys-slang contributors
// SPDX-License-Identifier: ISC

module prim_esc_rxtx_sva_harness (
  input logic clk_i,
  input logic rst_ni,
  input logic resp_err_pi,
  input logic resp_err_ni,
  input logic esc_err_pi,
  input logic esc_err_ni,
  input logic esc_req_i,
  input logic ping_req_i
);
  initial assume(!rst_ni);

  logic ping_ok;
  logic integ_fail;
  logic esc_req;

  prim_esc_rxtx_tb u_tb (
    .clk_i,
    .rst_ni,
    .resp_err_pi,
    .resp_err_ni,
    .esc_err_pi,
    .esc_err_ni,
    .esc_req_i,
    .ping_req_i,
    .ping_ok_o(ping_ok),
    .integ_fail_o(integ_fail),
    .esc_req_o(esc_req)
  );

  prim_esc_rxtx_assert_fpv #(
    .TimeoutCntDw(6)
  ) u_assert (
    .clk_i,
    .rst_ni,
    .resp_err_pi,
    .resp_err_ni,
    .esc_err_pi,
    .esc_err_ni,
    .esc_req_i,
    .ping_req_i,
    .ping_ok_o(ping_ok),
    .integ_fail_o(integ_fail),
    .esc_req_o(esc_req)
  );
endmodule
