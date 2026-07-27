module opentitan_arbiter_liveness_harness (
	(* gclk *) input logic clk_i
);
	localparam int unsigned N = 4;
	localparam int unsigned DW = 8;
	localparam int unsigned IdxW = prim_util_pkg::vbits(N);

	wire rst_ni = 1'b1;
	wire req_chk_i = 1'b1;
	wire [N-1:0] req_i = '1;
	wire [DW-1:0] data_i [N] = '{default: '0};
	wire ready_i = 1'b1;
	wire [N-1:0] gnt_o;
	wire [IdxW-1:0] idx_o;
	wire valid_o;
	wire [DW-1:0] data_o;

	prim_arbiter_ppc #(
		.N(N),
		.DW(DW),
		.EnDataPort(1'b1)
	) dut (
		.clk_i,
		.rst_ni,
		.req_chk_i,
		.req_i,
		.data_i,
		.gnt_o,
		.idx_o,
		.valid_o,
		.data_o,
		.ready_i
	);

`ifdef MUTATE_LIVENESS
	wire [N-1:0] observed_gnt = gnt_o & {{(N-1){1'b1}}, 1'b0};
`elsif MUTATE_SAFETY
	wire [N-1:0] observed_gnt = gnt_o | {{(N-2){1'b0}}, 2'b11};
`else
	wire [N-1:0] observed_gnt = gnt_o;
`endif

	grant_is_onehot: assert property (@(posedge clk_i)
		disable iff (!rst_ni) $onehot(observed_gnt));
	grant_has_request: assert property (@(posedge clk_i)
		disable iff (!rst_ni) |observed_gnt |-> |req_i);
	grant_has_ready: assert property (@(posedge clk_i)
		disable iff (!rst_ni) |observed_gnt |-> ready_i);

	// These are the NoStarvation_A obligations from prim_arbiter_ppc,
	// specialized to stable requests and a stable ready sink.
	no_starvation_0: assert property (@(posedge clk_i)
		disable iff (!rst_ni)
		ready_i && req_i[0] |-> strong(##[0:$] observed_gnt[0]));
	no_starvation_1: assert property (@(posedge clk_i)
		disable iff (!rst_ni)
		ready_i && req_i[1] |-> strong(##[0:$] observed_gnt[1]));
	no_starvation_2: assert property (@(posedge clk_i)
		disable iff (!rst_ni)
		ready_i && req_i[2] |-> strong(##[0:$] observed_gnt[2]));
	no_starvation_3: assert property (@(posedge clk_i)
		disable iff (!rst_ni)
		ready_i && req_i[3] |-> strong(##[0:$] observed_gnt[3]));

	grant_reachable_0: cover property (@(posedge clk_i)
		disable iff (!rst_ni) observed_gnt[0]);
	grant_reachable_1: cover property (@(posedge clk_i)
		disable iff (!rst_ni) observed_gnt[1]);
	grant_reachable_2: cover property (@(posedge clk_i)
		disable iff (!rst_ni) observed_gnt[2]);
	grant_reachable_3: cover property (@(posedge clk_i)
		disable iff (!rst_ni) observed_gnt[3]);
endmodule
