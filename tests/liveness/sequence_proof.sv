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
