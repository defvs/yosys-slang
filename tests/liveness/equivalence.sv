module liveness_equivalence (
	input logic clk,
	input logic goal,
	input logic fair_goal
);
	always @(posedge clk) begin
		direct_ref: assert(goal);
		fair_ref: assume(fair_goal);
	end

	direct_test: assert property (@(posedge clk) s_eventually goal);
	fair_test: assume property (@(posedge clk) s_eventually fair_goal);
endmodule
