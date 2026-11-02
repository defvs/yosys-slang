// No clock gets inferred from a process with no edge in its event control, so its
// assertions take the default clocking or a clock of their own. The conditions of the
// process decide on an attempt as of the tick of that clock (IEEE Std 1800-2017 sections
// 16.14.6 and 16.16).
module comb_clocking(input logic clk_i, input logic clk2_i, input logic c, input logic a,
					 input logic b);
	reg c_d1 = 0, a_d1 = 0, x_d1 = 0, t_d1 = 0;
	reg c_p1 = 0, a_p1 = 0;
	logic x;

	default clocking @(negedge clk_i); endclocking

	always @(negedge clk_i) begin
		boolean_ref: assert(!c || a);
		implication_ref: assert(!c_d1 || !a_d1 || b);
		const_ref: assert(!x_d1 || b);
		// Escaped to pair with the test in the named block below
		\with_automatic.automatic_ref : assert(!t_d1 || c);

		c_d1 <= c;
		a_d1 <= a;
		x_d1 <= a ^ c;
		t_d1 <= a & b;
	end

	always @(posedge clk2_i) begin
		explicit_ref: assert(!c_p1 || (a_p1 && b));

		c_p1 <= c;
		a_p1 <= a;
	end

	always_comb begin
		if (c) begin
			boolean_test: assert property (a);
			implication_test: assert property (a |=> b);
			explicit_test: assert property (@(posedge clk2_i) a ##1 b);
		end

		// The value the process gives it ahead of the assertion, whatever comes after
		x = a ^ c;
		const_test: assert property (const'(x) |=> b);
		x = 1'b0;
	end

	// An automatic variable is read as of the assertion too
	always @* begin : with_automatic
		automatic logic t;
		t = a & b;
		automatic_test: assert property (t |=> c);
		t = 1'b1;
	end
endmodule
