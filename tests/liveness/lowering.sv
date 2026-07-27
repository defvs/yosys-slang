module liveness_lowering (
	input logic clk,
	input logic rst_n,
	input logic request,
	input logic grant,
	input logic ready,
	input logic hold,
	input logic released
);
	property eventual_grant;
		@(posedge clk) disable iff (!rst_n)
			request |-> s_eventually [1:$] grant;
	endproperty

	direct_live: assert property (@(posedge clk)
		disable iff (!rst_n) s_eventually ready);
	named_live: assert property (eventual_grant);
	fair_ready: assume property (@(posedge clk)
		disable iff (!rst_n) s_eventually ready);
	unbounded_sequence: assert property (@(posedge clk)
		disable iff (!rst_n) request |->
			strong(##[0:$] grant));
	strong_until: assert property (@(posedge clk)
		disable iff (!rst_n) hold s_until released);
endmodule
