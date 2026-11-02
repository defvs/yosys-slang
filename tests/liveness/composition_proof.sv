// An eventuality of a sequence spelled strong(): a bare one would be weak in an
// assertion or an assumption (IEEE 1800-2017 16.12.2), which is rejected where its
// match is unbounded. The assumption makes `b` come again and again.
module strong_assumed_pass ((* gclk *) input logic clk, input logic a, input logic b);
	assume property (@(posedge clk) s_eventually strong(##[1:$] b));
	assert property (@(posedge clk) s_eventually b);
endmodule

// It says nothing of `a`
module strong_assumed_fail ((* gclk *) input logic clk, input logic a, input logic b);
	assume property (@(posedge clk) s_eventually strong(##[1:$] b));
	assert property (@(posedge clk) s_eventually (a && b));
endmodule

// The match of `a ##[1:$] b` from the `a` on phase 0 ends on phase 2
module strong_next_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 2;

	assert property (@(posedge clk) s_eventually (nexttime strong(a ##[1:$] b)));
endmodule

module strong_next_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = 1'b0;

	assert property (@(posedge clk) s_eventually (nexttime strong(a ##[1:$] b)));
endmodule

// weak() of a bounded sequence is strong(): no match of `a ##1 b` ever comes
module weak_bounded_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 2;

	assert property (@(posedge clk) s_eventually weak(a ##1 b));
endmodule

module strong_bounded_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 1;

	assert property (@(posedge clk) s_eventually strong(a ##1 b));
endmodule
