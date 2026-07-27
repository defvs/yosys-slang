module liveness_regular_sequences (
	input logic clk,
	input logic a,
	input logic b,
	input logic c
);
	prefixed_delay: assert property (@(posedge clk)
		strong(a ##[0:$] b));

	same_endpoint: assert property (@(posedge clk)
		strong((##[0:$] a) intersect (##[0:$] b)));

	both_endpoints: assert property (@(posedge clk)
		strong((##[0:$] a) and (##[0:$] b)));

	second_occurrence: assert property (@(posedge clk)
		strong(a[->2]));

	second_nonconsecutive: assert property (@(posedge clk)
		strong(a[=2]));

	guarded_wait: assert property (@(posedge clk)
		strong(a throughout (##[0:$] b)));

	contained_match: assert property (@(posedge clk)
		strong((a ##1 b) within (##[0:$] c)));

	nested_eventual: assert property (@(posedge clk)
		s_eventually (a ##[0:$] b));
endmodule
