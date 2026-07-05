// Copyright (c) 2026 The yosys-slang contributors
// SPDX-License-Identifier: ISC

module tlul_sva_harness (
  input logic clk_i,
  input logic rst_ni,
  input logic [$bits(tlul_pkg::tl_h2d_t)-1:0] h2d_i,
  input logic [$bits(tlul_pkg::tl_d2h_t)-1:0] d2h_i
);
  import tlul_pkg::*;

  tl_h2d_t h2d;
  tl_d2h_t d2h;

  assign h2d = tl_h2d_t'(h2d_i);
  assign d2h = tl_d2h_t'(d2h_i);

  tlul_assert #(
    .EndpointType("Device")
  ) u_tlul_assert (
    .clk_i,
    .rst_ni,
    .h2d,
    .d2h
  );

`ifdef TLUL_ASSUME_LEGAL_D2H
  assume_legal_d_param:
    assume property (@(posedge clk_i) disable iff (!rst_ni)
      d2h.d_valid |-> d2h.d_param == '0);

  assume_response_has_request:
    assume property (@(posedge clk_i) disable iff (!rst_ni)
      d2h.d_valid |-> (u_tlul_assert.curr_fwd | u_tlul_assert.pend_req[d2h.d_source].pend));

  assume_response_size_matches_request:
    assume property (@(posedge clk_i) disable iff (!rst_ni)
      d2h.d_valid |-> d2h.d_size ==
        (u_tlul_assert.curr_fwd ? u_tlul_assert.curr_req.size :
                                  u_tlul_assert.pend_req[d2h.d_source].size));

  assume_response_opcode_matches_request:
    assume property (@(posedge clk_i) disable iff (!rst_ni)
      d2h.d_valid |-> d2h.d_opcode ==
        (((u_tlul_assert.curr_fwd ? u_tlul_assert.curr_req.opcode :
                                    u_tlul_assert.pend_req[d2h.d_source].opcode) == 3'h4) ?
          3'h1 : 3'h0));
`endif

`ifdef TLUL_ASSUME_LEGAL_D2H_IMMEDIATE
  always_comb begin
    if (rst_ni && d2h.d_valid) begin
      assume(d2h.d_param == '0);
      assume(u_tlul_assert.curr_fwd | u_tlul_assert.pend_req[d2h.d_source].pend);
      assume(d2h.d_size == (u_tlul_assert.curr_fwd ? u_tlul_assert.curr_req.size :
                                                       u_tlul_assert.pend_req[d2h.d_source].size));
      assume(d2h.d_opcode ==
        (((u_tlul_assert.curr_fwd ? u_tlul_assert.curr_req.opcode :
                                    u_tlul_assert.pend_req[d2h.d_source].opcode) == 3'h4) ?
          3'h1 : 3'h0));
    end
  end
`endif
endmodule
