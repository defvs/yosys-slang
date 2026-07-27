module response_proof (
	(* gclk *) input logic clk,
	input logic request
);
	logic [1:0] age = 0;

	always_ff @(posedge clk) begin
		if (request && age != 3)
			age <= age + 1'b1;
	end

	assume_request: assume property (@(posedge clk) request);

`ifdef MUTATE_LIVENESS
	wire grant = 1'b0;
`else
	wire grant = age == 3;
`endif

	request_eventually_granted: assert property (@(posedge clk)
		request |-> s_eventually grant);
	request_is_reachable: cover property (@(posedge clk) request);
endmodule
