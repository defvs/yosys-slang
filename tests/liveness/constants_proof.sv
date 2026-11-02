// Constant expressions in the operands of the liveness monitors: enum literals,
// genvars, parameters overridden by defparam, hierarchical references and functions.
// A counter wraps around, so each value of it comes up again and again.

module constants_enum_pass ((* gclk *) input logic clk);
	typedef enum logic [1:0] { IDLE, BUSY, DONE, STUCK } state_e;
	logic [2:0] cnt = 0;
	always_ff @(posedge clk)
		cnt <= cnt + 1'b1;

	state_e st;
	assign st = cnt == 2 ? BUSY : cnt == 5 ? DONE : IDLE;

	assert property (@(posedge clk) (st == BUSY) |-> strong((st == BUSY) ##[1:$] (st == DONE)));
endmodule

module constants_enum_fail ((* gclk *) input logic clk);
	typedef enum logic [1:0] { IDLE, BUSY, DONE, STUCK } state_e;
	logic [2:0] cnt = 0;
	always_ff @(posedge clk)
		cnt <= cnt + 1'b1;

	state_e st;
	assign st = cnt == 2 ? BUSY : cnt == 5 ? DONE : IDLE;

	assert property (@(posedge clk) (st == BUSY) |-> strong((st == BUSY) ##[1:$] (st == STUCK)));
endmodule

module constants_defparam_sub #(parameter int LIMIT = 9) (input logic clk, input logic [2:0] cnt);
	assert property (@(posedge clk) s_eventually (cnt == LIMIT));
endmodule

// The default of the parameter is out of reach
module constants_defparam_pass ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	always_ff @(posedge clk)
		cnt <= cnt + 1'b1;

	constants_defparam_sub u(.clk, .cnt);
	defparam u.LIMIT = 5;
endmodule

module constants_defparam_fail ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	always_ff @(posedge clk)
		cnt <= cnt + 1'b1;

	constants_defparam_sub u(.clk, .cnt);
	defparam u.LIMIT = 8;
endmodule

module constants_hierarchical_sub #(parameter int LIMIT = 9) ();
endmodule

module constants_hierarchical_pass ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	always_ff @(posedge clk)
		cnt <= cnt + 1'b1;

	constants_hierarchical_sub u();
	defparam u.LIMIT = 5;

	assert property (@(posedge clk) s_eventually (cnt == u.LIMIT));
endmodule

module constants_hierarchical_fail ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	always_ff @(posedge clk)
		cnt <= cnt + 1'b1;

	constants_hierarchical_sub u();
	defparam u.LIMIT = 8;

	assert property (@(posedge clk) s_eventually (cnt == u.LIMIT));
endmodule

module constants_genvar_pass ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	always_ff @(posedge clk)
		cnt <= cnt + 1'b1;

	for (genvar i = 0; i < 3; i++) begin : gen
		assert property (@(posedge clk) s_eventually (cnt == i + 5));
	end
endmodule

// cnt == 8 is out of reach
module constants_genvar_fail ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	always_ff @(posedge clk)
		cnt <= cnt + 1'b1;

	for (genvar i = 0; i < 3; i++) begin : gen
		assert property (@(posedge clk) s_eventually (cnt == i + 6));
	end
endmodule

// Each call of a static function has arguments of its own
module constants_function_pass ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	always_ff @(posedge clk)
		cnt <= cnt + 1'b1;

	function logic same(logic y);
		return y;
	endfunction

	function automatic int fact(int n);
		return n <= 1 ? 1 : n * fact(n - 1);
	endfunction

	assert property (@(posedge clk) s_eventually (same(cnt == fact(3)) && !same(cnt == 3'd5)));
endmodule

module constants_function_fail ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	always_ff @(posedge clk)
		cnt <= cnt + 1'b1;

	function logic same(logic y);
		return y;
	endfunction

	function automatic int fact(int n);
		return n <= 1 ? 1 : n * fact(n - 1);
	endfunction

	assert property (@(posedge clk) s_eventually (same(cnt == fact(3)) && same(cnt == 3'd5)));
endmodule
