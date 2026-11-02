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

	assert property (@(posedge clk) request |-> s_eventually strong(s[*2]));
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

	assert property (@(posedge clk) request |-> s_eventually strong(s[*2]));
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

	assert property (@(posedge clk) request |-> s_eventually nexttime strong(a[*1:$]));
endmodule

module nexttime_empty_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;

	assert property (@(posedge clk) request |-> s_eventually nexttime strong(1'b0[*0:$]));
endmodule

// An empty match of a sequence used as a property is no success
module empty_eventually_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire b = age == 2;

	assert property (@(posedge clk) request |-> s_eventually (1'b0[*0:1] ##1 b));
endmodule

module empty_eventually_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;

	assert property (@(posedge clk) request |-> s_eventually 1'b0[*0:1]);
endmodule

// `R #=# P` is `(R ##1 `true) #-# P`, which an empty match of R matches on the attempt's
// tick, while `R #-# P` takes a non-empty match of R
module empty_followed_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire b = age == 2;

	assert property (@(posedge clk) request |-> (1'b0[*0:1] #=# s_eventually b));
endmodule

module empty_followed_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire b = 1'b0;

	assert property (@(posedge clk) request |-> (1'b0[*0:1] #=# s_eventually b));
endmodule

module empty_overlapped_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire b = age == 2;

	assert property (@(posedge clk) request |-> (1'b0[*0:1] #-# s_eventually b));
endmodule

// `##n empty` is `##(n-1) `true`, so the empty match of `a[*0:1]` ends a tick ahead of the
// delay, and a reject that comes after that tick comes too late
module suffix_empty_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire a = 1'b0;
	wire reject = age == 2;

	assert property (@(posedge clk) request |-> reject_on (reject) strong(##[2:$] a[*0:1]));
endmodule

module suffix_empty_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire a = 1'b0;
	wire reject = age == 1;

	assert property (@(posedge clk) request |-> reject_on (reject) strong(##[2:$] a[*0:1]));
endmodule
