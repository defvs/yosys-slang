// What holds of the attempt three ticks back, by the first of its ticks with `r`: the
// ticks before that one, extended by letters which satisfy any boolean, satisfy the
// property (IEEE Std 1800-2017 Annex F). The letters an antecedent sees, and those of
// a negated property, satisfy none.
module disable_each_tick(input clk_i, input logic r, input logic a, input logic b,
						 input logic c, input logic d);
	reg gate1 = 0;
	reg gate2 = 0;
	reg gate3 = 0;
	reg r_d1, r_d2, r_d3;
	reg a_d1, a_d2, a_d3;
	reg b_d1, b_d2, b_d3;
	reg c_d1, c_d2, c_d3;

	wire sequence_holds = r_d3 ? 1'b1
		: r_d2 ? a_d3
		: r_d1 ? a_d3 && b_d2
		: r ? a_d3 && b_d2 && c_d1
		: a_d3 && b_d2 && c_d1 && d;

	wire implication_holds = r_d3 ? 1'b1
		: r_d2 ? !a_d3 || b_d3
		: r_d1 ? !a_d3 || (b_d3 && c_d2)
		: r ? !a_d3 || (b_d3 && c_d2)
		: !a_d3 || (b_d3 && c_d2 && d);

	wire negation_holds = r_d3 ? 1'b1
		: r_d2 ? 1'b1
		: r_d1 ? !(a_d3 && b_d2)
		: r ? !(a_d3 && b_d2)
		: !(a_d3 && b_d2) && !(c_d3 && d);

	always @(posedge clk_i) begin
		if (gate3) begin
			sequence_ref: assert(sequence_holds);
			implication_ref: assert(implication_holds);
			negation_ref: assert(negation_holds);
		end

		gate1 <= 1;
		gate2 <= gate1;
		gate3 <= gate2;
		r_d1 <= r;
		r_d2 <= r_d1;
		r_d3 <= r_d2;
		a_d1 <= a;
		a_d2 <= a_d1;
		a_d3 <= a_d2;
		b_d1 <= b;
		b_d2 <= b_d1;
		b_d3 <= b_d2;
		c_d1 <= c;
		c_d2 <= c_d1;
		c_d3 <= c_d2;
	end

	sequence_test: assert property(@(posedge clk_i) disable iff (r) a ##1 b ##1 c ##1 d);
	implication_test: assert property(@(posedge clk_i) disable iff (r) a |-> (b ##1 c ##2 d));
	negation_test: assert property(@(posedge clk_i) disable iff (r) not ((a ##1 b) or (c ##3 d)));
endmodule
