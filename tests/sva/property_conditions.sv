// The items of a property case compare by case equality (IEEE Std 1800-2017
// sections 12.5 and 16.12.16), an x in one is no wildcard.
module property_conditions(input logic clk, input logic b, input logic [1:0] m,
						   input logic [1:0] n);
	always @(posedge clk) begin
		// No value of m is 2'b1x by case equality
		case_x_ref: assert(1'b1);
		case_x_test: assert property (case (m) 2'b1x: b; endcase);

		case_signal_ref: assert(m != n || b);
		case_signal_test: assert property (case (m) n: b; endcase);
	end
endmodule
