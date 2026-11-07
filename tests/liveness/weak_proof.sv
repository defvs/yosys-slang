// An attempt of `t throughout (##[0:$] x ##6 y)` waits on several `x` at once, so the
// assertion checks one attempt the solver picks. It has to be able to pick the one that
// fails, here the second, while the first has matched.
module weak_selected_pass ((* gclk *) input logic clk);
	logic [3:0] age = 0;
	always_ff @(posedge clk)
		if (age != 15)
			age <= age + 1'b1;

	wire a = age == 0 || age == 2;
	wire x = age == 1 || age == 3;
	wire y = age == 7 || age == 9;
	wire t = age <= 9;

	assert property (@(posedge clk) a |-> t throughout (##[0:$] x ##6 y));
endmodule

module weak_selected_fail ((* gclk *) input logic clk);
	logic [3:0] age = 0;
	always_ff @(posedge clk)
		if (age != 15)
			age <= age + 1'b1;

	wire a = age == 0 || age == 2;
	wire x = age == 1 || age == 3;
	wire y = age == 7;
	wire t = age <= 9;

	assert property (@(posedge clk) a |-> t throughout (##[0:$] x ##6 y));
endmodule

// An assumption of a weak sequence holds `b` up to the first `c` and leaves it free
// from there
module weak_assume_pass ((* gclk *) input logic clk, input logic a, input logic b,
						 input logic c);
	assume property (@(posedge clk) a |-> b throughout c[->1]);
	assert property (@(posedge clk) a && !c |=> b);
endmodule

module weak_assume_fail ((* gclk *) input logic clk, input logic a, input logic b,
						 input logic c);
	assume property (@(posedge clk) a |-> b throughout c[->1]);
	assert property (@(posedge clk) a |=> b);
endmodule
