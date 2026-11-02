// A repetition of a named sequence repeats all of it: `s[*2]` is `s ##1 s`
module named_strong_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire a = age == 2 || age == 4;

	sequence s;
		##[1:$] a;
	endsequence

	assert property (@(posedge clk) request |-> strong(s[*2]));
endmodule

module named_strong_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire a = age == 2;

	sequence s;
		##[1:$] a;
	endsequence

	assert property (@(posedge clk) request |-> strong(s[*2]));
endmodule

module named_eventually_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire a = age == 2 || age == 4;

	sequence s;
		##[1:$] a;
	endsequence

	assert property (@(posedge clk) request |-> s_eventually s[*2]);
endmodule

module named_eventually_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire a = age == 2;

	sequence s;
		##[1:$] a;
	endsequence

	assert property (@(posedge clk) request |-> s_eventually s[*2]);
endmodule

// A leading `##0` keeps only the non-empty matches of what follows. Taking the empty
// match of `a[*0:$]` would make it `b` on the request tick.
module zero_prefix_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire a = age == 0;
	wire b = age == 1;

	assert property (@(posedge clk) request |-> strong((##0 a[*0:$]) ##1 b));
endmodule

module zero_prefix_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire a = 1'b0;
	wire b = age == 0;

	assert property (@(posedge clk) request |-> strong((##0 a[*0:$]) ##1 b));
endmodule

// The operand of `nexttime` is a property, an empty match of it is no success
module nexttime_empty_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire a = age == 3;

	assert property (@(posedge clk) request |-> s_eventually nexttime (a[*1:$]));
endmodule

module nexttime_empty_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;

	assert property (@(posedge clk) request |-> s_eventually nexttime (1'b0[*0:$]));
endmodule
