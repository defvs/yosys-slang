// Ranged operands placed after the paths of a range all at once, each against the attempt
// spelled out tick by tick: repetitions, first_match, throughout and `or`, after a range,
// after a leading range and after an empty match
module placed_ranges(input clk_i, input logic a, input logic b, input logic c, input logic d,
					 input logic e);
	sequence pair(x, y);
		x ##1 y;
	endsequence

	reg gate1 = 0;
	reg gate2 = 0;
	reg gate3 = 0;
	reg gate4 = 0;
	reg gate5 = 0;
	reg gate6 = 0;
	reg a_d1, a_d2, a_d3, a_d4, a_d5, a_d6;
	reg b_d1, b_d2, b_d3, b_d4, b_d5, b_d6;
	reg c_d1, c_d2, c_d3, c_d4, c_d5, c_d6;
	reg d_d1, d_d2, d_d3, d_d4, d_d5, d_d6;
	reg e_d1, e_d2, e_d3, e_d4, e_d5, e_d6;

	always @(posedge clk_i) begin
		// A ranged repetition of a boolean
		repetition_ref: assert(!gate4
				|| (!a_d4
					|| (b_d4 && c_d3 && d_d2)
					|| (b_d4 && c_d3 && c_d2 && d_d1)
					|| (b_d4 && c_d2 && d_d1)
					|| (b_d4 && c_d2 && c_d1 && d)));
		// With a least count above one
		repetition_fewest_ref: assert(!gate5
				|| (!a_d5
					|| (b_d5 && c_d4 && c_d3 && d_d2)
					|| (b_d5 && c_d4 && c_d3 && c_d2 && d_d1)
					|| (b_d5 && c_d3 && c_d2 && d_d1)
					|| (b_d5 && c_d3 && c_d2 && c_d1 && d)));
		// With an empty match, which leaves the range before it
		repetition_empty_ref: assert(!gate4
				|| (!a_d4
					|| (b_d4 && c_d3 && d_d2)
					|| (b_d4 && c_d3 && c_d2 && d_d1)
					|| (b_d4 && d_d3)
					|| (b_d4 && c_d2 && d_d1)
					|| (b_d4 && c_d2 && c_d1 && d)
					|| (b_d4 && d_d2)));
		// Of a sequence of two ticks
		repetition_stride_ref: assert(!gate6
				|| (!a_d6
					|| (b_d6 && c_d5 && d_d4 && e_d3)
					|| (b_d6 && c_d5 && d_d4 && c_d3 && d_d2 && e_d1)
					|| (b_d6 && c_d4 && d_d3 && e_d2)
					|| (b_d6 && c_d4 && d_d3 && c_d2 && d_d1 && e)));
		// Of a named sequence of two ticks
		repetition_named_ref: assert(!gate6
				|| (!a_d6
					|| (b_d6 && c_d5 && d_d4 && e_d3)
					|| (b_d6 && c_d5 && d_d4 && c_d3 && d_d2 && e_d1)
					|| (b_d6 && c_d4 && d_d3 && e_d2)
					|| (b_d6 && c_d4 && d_d3 && c_d2 && d_d1 && e)));
		// Of a sequence of a range
		repetition_ranged_ref: assert(!gate6
				|| (!a_d6
					|| (b_d6 && c_d5 && d_d5 && e_d4)
					|| (b_d6 && c_d5 && d_d4 && e_d3)
					|| (b_d6 && c_d5 && d_d5 && c_d4 && d_d4 && e_d3)
					|| (b_d6 && c_d5 && d_d5 && c_d4 && d_d3 && e_d2)
					|| (b_d6 && c_d5 && d_d4 && c_d3 && d_d3 && e_d2)
					|| (b_d6 && c_d5 && d_d4 && c_d3 && d_d2 && e_d1)
					|| (b_d6 && c_d4 && d_d4 && e_d3)
					|| (b_d6 && c_d4 && d_d3 && e_d2)
					|| (b_d6 && c_d4 && d_d4 && c_d3 && d_d3 && e_d2)
					|| (b_d6 && c_d4 && d_d4 && c_d3 && d_d2 && e_d1)
					|| (b_d6 && c_d4 && d_d3 && c_d2 && d_d2 && e_d1)
					|| (b_d6 && c_d4 && d_d3 && c_d2 && d_d1 && e)));
		// Two in a row
		repetitions_ref: assert(!gate4
				|| (!a_d4
					|| (b_d4 && c_d3 && d_d2)
					|| (b_d4 && c_d3 && c_d2 && d_d1)
					|| (b_d4 && b_d3 && c_d2 && d_d1)
					|| (b_d4 && b_d3 && c_d2 && c_d1 && d)));
		// After a range with nothing before it
		leading_repetition_ref: assert(!gate4
				|| (!a_d4
					|| (c_d3 && d_d2)
					|| (c_d3 && c_d2 && d_d1)
					|| (c_d2 && d_d1)
					|| (c_d2 && c_d1 && d)));
		// A concatenation after a range with nothing before it
		leading_concat_ref: assert(!gate4
				|| (!a_d4 || (c_d3 && d_d2) || (c_d3 && d_d1) || (c_d2 && d_d1) || (c_d2 && d)));
		// After a range following an empty match
		empty_before_ref: assert(!gate4
				|| (!a_d4
					|| (c_d4 && d_d3)
					|| (c_d4 && c_d3 && d_d2)
					|| (c_d3 && d_d2)
					|| (c_d3 && c_d2 && d_d1)
					|| (b_d4 && c_d3 && d_d2)
					|| (b_d4 && c_d3 && c_d2 && d_d1)
					|| (b_d4 && c_d2 && d_d1)
					|| (b_d4 && c_d2 && c_d1 && d)));
		// As the first element of a concatenation
		first_element_ref: assert(!gate4
				|| (!a_d4
					|| (b_d4 && c_d3 && d_d2)
					|| (b_d4 && c_d3 && c_d2 && d_d1)
					|| (b_d4 && c_d2 && d_d1)
					|| (b_d4 && c_d2 && c_d1 && d)));
		// First_match of a range
		first_match_ref: assert(!gate5
				|| (!a_d5
					|| (b_d5 && c_d4 && d_d3 && e_d2)
					|| (b_d5 && c_d4 && d_d2 && !(c_d4 && d_d3) && e_d1)
					|| (b_d5 && c_d3 && d_d2 && e_d1)
					|| (b_d5 && c_d3 && d_d1 && !(c_d3 && d_d2) && e)));
		// First_match of a leading range
		first_match_leading_ref: assert(!gate5
				|| (!a_d5
					|| (b_d5 && d_d3 && e_d2)
					|| (b_d5 && d_d2 && !d_d3 && e_d1)
					|| (b_d5 && d_d2 && e_d1)
					|| (b_d5 && d_d1 && !d_d2 && e)));
		// First_match of a range after fixed delays
		first_match_chain_ref: assert(!gate5
				|| (!a_d5
					|| (b_d5 && c_d4 && d_d3 && e_d3)
					|| (b_d5 && c_d4 && d_d3 && e_d2 && !(c_d4 && d_d3 && e_d3))
					|| (b_d5
						&& c_d4
						&& d_d3
						&& e_d1
						&& !((c_d4 && d_d3 && e_d3) || (c_d4 && d_d3 && e_d2)))
					|| (b_d5 && c_d3 && d_d2 && e_d2)
					|| (b_d5 && c_d3 && d_d2 && e_d1 && !(c_d3 && d_d2 && e_d2))
					|| (b_d5
						&& c_d3
						&& d_d2
						&& e
						&& !((c_d3 && d_d2 && e_d2) || (c_d3 && d_d2 && e_d1)))));
		// Throughout of a range
		throughout_ref: assert(!gate5
				|| (!a_d5
					|| (b_d5 && c_d4 && d_d3 && e_d4 && e_d3 && a_d2)
					|| (b_d5 && c_d4 && d_d2 && e_d4 && e_d3 && e_d2 && a_d1)
					|| (b_d5 && c_d3 && d_d2 && e_d3 && e_d2 && a_d1)
					|| (b_d5 && c_d3 && d_d1 && e_d3 && e_d2 && e_d1 && a)));
		// Throughout of a ranged repetition
		throughout_repetition_ref: assert(!gate5
				|| (!a_d5
					|| (b_d5 && c_d4 && e_d4 && d_d3)
					|| (b_d5 && c_d4 && c_d3 && e_d4 && e_d3 && d_d2)
					|| (b_d5 && c_d4 && c_d3 && c_d2 && e_d4 && e_d3 && e_d2 && d_d1)
					|| (b_d5 && c_d3 && e_d3 && d_d2)
					|| (b_d5 && c_d3 && c_d2 && e_d3 && e_d2 && d_d1)
					|| (b_d5 && c_d3 && c_d2 && c_d1 && e_d3 && e_d2 && e_d1 && d)));
		// Throughout of first_match
		throughout_first_match_ref: assert(!gate4
				|| (!a_d4
					|| (b_d4 && c_d3 && d_d2 && e_d3 && e_d2)
					|| (b_d4 && c_d3 && d_d1 && !(c_d3 && d_d2) && e_d3 && e_d2 && e_d1)
					|| (b_d4 && c_d2 && d_d1 && e_d2 && e_d1)
					|| (b_d4 && c_d2 && d && !(c_d2 && d_d1) && e_d2 && e_d1 && e)));
		// Throughout of throughout
		throughout_nested_ref: assert(!gate4
				|| (!a_d4
					|| (b_d4 && c_d3 && d_d2 && a_d3 && a_d2 && e_d3 && e_d2)
					|| (b_d4 && c_d3 && d_d1 && a_d3 && a_d2 && a_d1 && e_d3 && e_d2 && e_d1)
					|| (b_d4 && c_d2 && d_d1 && a_d2 && a_d1 && e_d2 && e_d1)
					|| (b_d4 && c_d2 && d && a_d2 && a_d1 && a && e_d2 && e_d1 && e)));
		// Throughout of ranges in a row
		throughout_chain_ref: assert(!gate6
				|| (!a_d6
					|| (b_d6 && c_d5 && d_d4 && a_d4 && e_d5 && e_d4)
					|| (b_d6 && c_d5 && d_d4 && b_d4 && e_d5 && e_d4)
					|| (b_d6 && c_d5 && d_d4 && a_d3 && e_d5 && e_d4 && e_d3)
					|| (b_d6 && c_d5 && d_d4 && b_d3 && e_d5 && e_d4 && e_d3)
					|| (b_d6 && c_d5 && d_d3 && a_d3 && e_d5 && e_d4 && e_d3)
					|| (b_d6 && c_d5 && d_d3 && b_d3 && e_d5 && e_d4 && e_d3)
					|| (b_d6 && c_d5 && d_d3 && a_d2 && e_d5 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d5 && d_d3 && b_d2 && e_d5 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d5 && c_d4 && d_d3 && a_d3 && e_d5 && e_d4 && e_d3)
					|| (b_d6 && c_d5 && c_d4 && d_d3 && b_d3 && e_d5 && e_d4 && e_d3)
					|| (b_d6 && c_d5 && c_d4 && d_d3 && a_d2 && e_d5 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d5 && c_d4 && d_d3 && b_d2 && e_d5 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d5 && c_d4 && d_d2 && a_d2 && e_d5 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d5 && c_d4 && d_d2 && b_d2 && e_d5 && e_d4 && e_d3 && e_d2)
					|| (b_d6
						&& c_d5
						&& c_d4
						&& d_d2
						&& a_d1
						&& e_d5
						&& e_d4
						&& e_d3
						&& e_d2
						&& e_d1)
					|| (b_d6
						&& c_d5
						&& c_d4
						&& d_d2
						&& b_d1
						&& e_d5
						&& e_d4
						&& e_d3
						&& e_d2
						&& e_d1)
					|| (b_d6 && c_d4 && d_d3 && a_d3 && e_d4 && e_d3)
					|| (b_d6 && c_d4 && d_d3 && b_d3 && e_d4 && e_d3)
					|| (b_d6 && c_d4 && d_d3 && a_d2 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d4 && d_d3 && b_d2 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d4 && d_d2 && a_d2 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d4 && d_d2 && b_d2 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d4 && d_d2 && a_d1 && e_d4 && e_d3 && e_d2 && e_d1)
					|| (b_d6 && c_d4 && d_d2 && b_d1 && e_d4 && e_d3 && e_d2 && e_d1)
					|| (b_d6 && c_d4 && c_d3 && d_d2 && a_d2 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d4 && c_d3 && d_d2 && b_d2 && e_d4 && e_d3 && e_d2)
					|| (b_d6 && c_d4 && c_d3 && d_d2 && a_d1 && e_d4 && e_d3 && e_d2 && e_d1)
					|| (b_d6 && c_d4 && c_d3 && d_d2 && b_d1 && e_d4 && e_d3 && e_d2 && e_d1)
					|| (b_d6 && c_d4 && c_d3 && d_d1 && a_d1 && e_d4 && e_d3 && e_d2 && e_d1)
					|| (b_d6 && c_d4 && c_d3 && d_d1 && b_d1 && e_d4 && e_d3 && e_d2 && e_d1)
					|| (b_d6 && c_d4 && c_d3 && d_d1 && a && e_d4 && e_d3 && e_d2 && e_d1 && e)
					|| (b_d6 && c_d4 && c_d3 && d_d1 && b && e_d4 && e_d3 && e_d2 && e_d1 && e)));
		// Or of ranges
		or_ref: assert(!gate5
				|| (!a_d5
					|| (b_d5 && c_d4 && a_d3)
					|| (b_d5 && c_d4 && c_d3 && a_d2)
					|| (b_d5 && d_d4 && e_d3 && a_d2)
					|| (b_d5 && d_d4 && e_d2 && a_d1)
					|| (b_d5 && c_d3 && a_d2)
					|| (b_d5 && c_d3 && c_d2 && a_d1)
					|| (b_d5 && d_d3 && e_d2 && a_d1)
					|| (b_d5 && d_d3 && e_d1 && a)));
		// In an antecedent
		antecedent_ref: assert(!gate3
				|| ((!(a_d3 && b_d2) || c_d2)
					&& (!(a_d3 && b_d2 && b_d1) || c_d1)
					&& (!(a_d3 && b_d1) || c_d1)
					&& (!(a_d3 && b_d1 && b) || c)));
		// Throughout in an antecedent
		antecedent_throughout_ref: assert(!gate5
				|| ((!(a_d5 && b_d4 && c_d3 && e_d4 && e_d3) || d_d2)
					&& (!(a_d5 && b_d4 && c_d2 && e_d4 && e_d3 && e_d2) || d_d1)
					&& (!(a_d5 && b_d3 && c_d2 && e_d3 && e_d2) || d_d1)
					&& (!(a_d5 && b_d3 && c_d1 && e_d3 && e_d2 && e_d1) || d)));
		// A cover
		cover_repetition_ref: cover(gate4
				&& ((a_d4 && b_d3 && c_d2)
					|| (a_d4 && b_d3 && b_d2 && c_d1)
					|| (a_d4 && b_d2 && c_d1)
					|| (a_d4 && b_d2 && b_d1 && c)));
		// First_match in a cover
		cover_first_match_ref: cover(gate4
				&& ((a_d4 && b_d3 && c_d2)
					|| (a_d4 && b_d3 && c_d1 && !(b_d3 && c_d2))
					|| (a_d4 && b_d2 && c_d1)
					|| (a_d4 && b_d2 && c && !(b_d2 && c_d1))));

		gate1 <= 1;
		gate2 <= gate1;
		gate3 <= gate2;
		gate4 <= gate3;
		gate5 <= gate4;
		gate6 <= gate5;
		a_d1 <= a;
		a_d2 <= a_d1;
		a_d3 <= a_d2;
		a_d4 <= a_d3;
		a_d5 <= a_d4;
		a_d6 <= a_d5;
		b_d1 <= b;
		b_d2 <= b_d1;
		b_d3 <= b_d2;
		b_d4 <= b_d3;
		b_d5 <= b_d4;
		b_d6 <= b_d5;
		c_d1 <= c;
		c_d2 <= c_d1;
		c_d3 <= c_d2;
		c_d4 <= c_d3;
		c_d5 <= c_d4;
		c_d6 <= c_d5;
		d_d1 <= d;
		d_d2 <= d_d1;
		d_d3 <= d_d2;
		d_d4 <= d_d3;
		d_d5 <= d_d4;
		d_d6 <= d_d5;
		e_d1 <= e;
		e_d2 <= e_d1;
		e_d3 <= e_d2;
		e_d4 <= e_d3;
		e_d5 <= e_d4;
		e_d6 <= e_d5;
	end

	repetition_test: assert property(@(posedge clk_i) a |-> b ##[1:2] c[*1:2] ##1 d);
	repetition_fewest_test: assert property(@(posedge clk_i) a |-> b ##[1:2] c[*2:3] ##1 d);
	repetition_empty_test: assert property(@(posedge clk_i) a |-> b ##[1:2] c[*0:2] ##1 d);
	repetition_stride_test: assert property(@(posedge clk_i) a |-> b ##[1:2] (c ##1 d)[*1:2] ##1 e);
	repetition_named_test: assert property(@(posedge clk_i) a |-> b ##[1:2] pair(c, d)[*1:2] ##1 e);
	repetition_ranged_test: assert property(@(posedge clk_i)
		a |-> b ##[1:2] (c ##[0:1] d)[*1:2] ##1 e);
	repetitions_test: assert property(@(posedge clk_i) a |-> b[*1:2] ##1 c[*1:2] ##1 d);
	leading_repetition_test: assert property(@(posedge clk_i) a |-> ##[1:2] c[*1:2] ##1 d);
	leading_concat_test: assert property(@(posedge clk_i) a |-> ##[1:2] (c ##[1:2] d));
	empty_before_test: assert property(@(posedge clk_i) a |-> b[*0:1] ##[1:2] c[*1:2] ##1 d);
	first_element_test: assert property(@(posedge clk_i) a |-> b ##[1:2] (c[*1:2] ##1 d));
	first_match_test: assert property(@(posedge clk_i)
		a |-> b ##[1:2] first_match(c ##[1:2] d) ##1 e);
	first_match_leading_test: assert property(@(posedge clk_i)
		a |-> b ##[1:2] first_match(##[1:2] d) ##1 e);
	first_match_chain_test: assert property(@(posedge clk_i)
		a |-> b ##[1:2] first_match(c ##1 d ##[0:2] e));
	throughout_test: assert property(@(posedge clk_i)
		a |-> b ##[1:2] (e throughout (c ##[1:2] d)) ##1 a);
	throughout_repetition_test: assert property(@(posedge clk_i)
		a |-> b ##[1:2] (e throughout c[*1:3]) ##1 d);
	throughout_first_match_test: assert property(@(posedge clk_i)
		a |-> b ##[1:2] (e throughout first_match(c ##[1:2] d)));
	throughout_nested_test: assert property(@(posedge clk_i)
		a |-> b ##[1:2] (e throughout (a throughout (c ##[1:2] d))));
	throughout_chain_test: assert property(@(posedge clk_i)
		a |-> b ##[1:2] (e throughout (c[*1:2] ##[1:2] d ##[0:1] (a or b))));
	or_test: assert property(@(posedge clk_i) a |-> b ##[1:2] (c[*1:2] or (d ##[1:2] e)) ##1 a);
	antecedent_test: assert property(@(posedge clk_i) a ##[1:2] b[*1:2] |-> c);
	antecedent_throughout_test: assert property(@(posedge clk_i)
		a ##[1:2] (e throughout (b ##[1:2] c)) |=> d);
	cover_repetition_test: cover property(@(posedge clk_i) a ##[1:2] b[*1:2] ##1 c);
	cover_first_match_test: cover property(@(posedge clk_i) a ##[1:2] first_match(b ##[1:2] c));
endmodule
