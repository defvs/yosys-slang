module never_match(input clk_i, input logic a, input logic b, input logic c, input logic d);
	always @(posedge clk_i) begin
		sequence_ref: assert(0);
		consequent_ref: assert(!a);
		antecedent_ref: assert(1);
		negated_ref: assert(1);
	end

	// Operands of different lengths leave no possible match
	sequence_test: assert property(@(posedge clk_i) (a ##1 b) intersect (c ##2 d));
	consequent_test: assert property(@(posedge clk_i) a |-> ((b ##1 c) intersect (c ##2 d)));
	antecedent_test: assert property(@(posedge clk_i) ((b ##1 c) intersect (c ##2 d)) |-> a);
	negated_test: assert property(@(posedge clk_i) not ((a ##1 b) intersect (c ##2 d)));
endmodule
