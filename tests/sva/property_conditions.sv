// The condition of a property if or case, and that of a synchronous abort, is a
// sampled value like the other operands (IEEE Std 1800-2017 sections 16.5.1,
// 16.12.14 and 16.12.16), a blocking assignment ahead of the assertion doesn't
// show in it. The items of a property case compare by case equality, an x in
// one is no wildcard.
module property_conditions(input logic clk, input logic a, input logic b,
						   input logic [1:0] m, input logic [1:0] n);
	logic y = 0;
	reg a_d1 = 0;

	always @(posedge clk) begin
		y = a;

		if_ref: assert(!a_d1 || b);
		if_test: assert property (if (y) b);

		case_ref: assert(!a_d1 || b);
		case_test: assert property (case (y) 1'b1: b; endcase);

		sync_accept_ref: assert(a_d1 || b);
		sync_accept_test: assert property (sync_accept_on (y) b);

		// No value of m is 2'b1x by case equality
		case_x_ref: assert(1'b1);
		case_x_test: assert property (case (m) 2'b1x: b; endcase);

		case_signal_ref: assert(m != n || b);
		case_signal_test: assert property (case (m) n: b; endcase);

		a_d1 <= a;
	end
endmodule
