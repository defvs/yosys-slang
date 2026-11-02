// A disable doesn't cancel what's settled ahead of it. The ticks of an attempt before
// the first one with the condition are extended by letters which satisfy any boolean,
// and the property holds if it holds on that (IEEE Std 1800-2017 Annex F). Under `not`
// the letters swap for ones which satisfy no boolean, as do those an antecedent sees.
module disable_settled_implication(input clk_i, input logic r, input logic a, input logic b,
								   input logic c);
	reg gate1 = 0;
	reg gate2 = 0;
	reg gate3 = 0;
	reg r_d1, r_d2, r_d3;
	reg a_d1, a_d2, a_d3;
	reg b_d1, b_d2;
	reg c_d1, c_d2, c_d3;

	always @(posedge clk_i) begin
		consequent_ref: assert(!gate1 || r_d1 || !a_d1 || (c_d1 && (r || b)));
		antecedent_range_ref: assert(!gate1 || r_d1 || ((!a_d1 || !b_d1 || c_d1) && (r || !a_d1 || !b || c)));

		// A disable cuts the antecedent short of a match
		antecedent_ref: assert(!gate1 || r_d1 || r || !a_d1 || !b || c);
		nonoverlap_ref: assert(!gate2 || r_d2 || r_d1 || !a_d2 || (b_d1 && (r || c)));
		nonoverlap_antecedent_ref: assert(!gate2 || r_d2 || r_d1 || r || !a_d2 || !b_d1 || c);

		// Only a match completed ahead of the disable fails a negation
		not_ref: assert(!gate1 || r_d1 || ((!a_d1 || !b_d1) && (r || !a_d1 || !b)));
		not_lengths_ref: assert(!gate3 || r_d3 || r_d2 || ((!a_d3 || !b_d2) && (r_d1 || r || !c_d3 || !b)));
		not_not_ref: assert(!gate1 || r_d1 || (a_d1 && (r || b)));
		not_implication_ref: assert(!gate1 || r_d1 || (a_d1 && (r || !b_d1 || !c)));
		not_negated_consequent_ref: assert(!gate1 || r_d1 || (a_d1 && (r || (b && c))));
		negated_consequent_ref: assert(!gate1 || r_d1 || r || !a_d1 || !b_d1 || !c);

		eventually_ref: assert(!gate2 || r_d2 || r_d1 || !c_d2 || (a_d2 && b_d1) || (a_d1 && (r || b)));
		and_ref: assert(!gate1 || r_d1 || (a_d1 && (!c_d1 || b_d1) && (r || b)));

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
		c_d1 <= c;
		c_d2 <= c_d1;
		c_d3 <= c_d2;
	end

	consequent_test: assert property(@(posedge clk_i) disable iff (r) a |-> (c ##1 b));
	antecedent_range_test: assert property(@(posedge clk_i) disable iff (r) (a ##[0:1] b) |-> c);
	antecedent_test: assert property(@(posedge clk_i) disable iff (r) (a ##1 b) |-> c);
	nonoverlap_test: assert property(@(posedge clk_i) disable iff (r) a |=> (b ##1 c));
	nonoverlap_antecedent_test: assert property(@(posedge clk_i) disable iff (r) (a ##1 b) |=> c);
	not_test: assert property(@(posedge clk_i) disable iff (r) not (a ##[0:1] b));
	not_lengths_test: assert property(@(posedge clk_i) disable iff (r) not ((a ##1 b) or (c ##3 b)));
	not_not_test: assert property(@(posedge clk_i) disable iff (r) not (not (a ##1 b)));
	not_implication_test: assert property(@(posedge clk_i) disable iff (r) not (a |-> (b ##1 c)));
	not_negated_consequent_test: assert property(@(posedge clk_i) disable iff (r) not ((a ##1 b) |-> not c));
	negated_consequent_test: assert property(@(posedge clk_i) disable iff (r) a |-> not (b ##1 c));
	eventually_test: assert property(@(posedge clk_i) disable iff (r) c |-> s_eventually [0:1] (a ##1 b));
	and_test: assert property(@(posedge clk_i) disable iff (r) (a ##1 b) and (c |-> b));
endmodule
