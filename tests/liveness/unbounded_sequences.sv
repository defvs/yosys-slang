module unbounded_sequences (
	input logic clk,
	input logic reset,
	input logic request,
	input logic a,
	input logic b
);
	sequence eventual_pair;
		##[0:$] (a ##1 b);
	endsequence

	named_suffix: assert property (@(posedge clk)
		disable iff (reset) request |-> strong(eventual_pair));

	repeated_suffix: assert property (@(posedge clk)
		request |=> strong(##[1:$] a[*2]));

	alternative_suffix: assert property (@(posedge clk)
		request |-> strong((##[0:$] a) or (##[0:$] b)));
endmodule
