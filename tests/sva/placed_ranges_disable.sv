// Ranged operands placed after the paths of a range all at once, each against the attempt
// spelled out tick by tick, under a disable, with the letters of Annex F for the booleans
// (see disable_settled_sequence.sv)
module placed_ranges_disable(input clk_i, input logic a, input logic b, input logic c,
							 input logic d, input logic e, input logic r);
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
	reg r_d1, r_d2, r_d3, r_d4, r_d5, r_d6;

	// The disable on any tick up to tick u of an attempt started L ticks ago, disL_u
	wire dis3_0 = r_d3;
	wire dis3_1 = r_d2 || dis3_0;
	wire dis3_2 = r_d1 || dis3_1;
	wire dis3_3 = r || dis3_2;
	wire dis4_0 = r_d4;
	wire dis4_1 = r_d3 || dis4_0;
	wire dis4_2 = r_d2 || dis4_1;
	wire dis4_3 = r_d1 || dis4_2;
	wire dis4_4 = r || dis4_3;
	wire dis5_0 = r_d5;
	wire dis5_1 = r_d4 || dis5_0;
	wire dis5_2 = r_d3 || dis5_1;
	wire dis5_3 = r_d2 || dis5_2;
	wire dis5_4 = r_d1 || dis5_3;
	wire dis5_5 = r || dis5_4;
	wire dis6_0 = r_d6;
	wire dis6_1 = r_d5 || dis6_0;
	wire dis6_2 = r_d4 || dis6_1;
	wire dis6_3 = r_d3 || dis6_2;
	wire dis6_4 = r_d2 || dis6_3;
	wire dis6_5 = r_d1 || dis6_4;
	wire dis6_6 = r || dis6_5;

	always @(posedge clk_i) begin
		// A ranged repetition of a boolean
		repetition_ref: assert(!gate4
				|| (!(a_d4 && !dis4_0)
					|| ((b_d4 || dis4_0) && (c_d3 || dis4_1) && (d_d2 || dis4_2))
					|| ((b_d4 || dis4_0)
						&& (c_d3 || dis4_1)
						&& (c_d2 || dis4_2)
						&& (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0) && (c_d2 || dis4_2) && (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0)
						&& (c_d2 || dis4_2)
						&& (c_d1 || dis4_3)
						&& (d || dis4_4))));
		// With a least count above one
		repetition_fewest_ref: assert(!gate5
				|| (!(a_d5 && !dis5_0)
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (c_d3 || dis5_2)
						&& (d_d2 || dis5_3))
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (c_d3 || dis5_2)
						&& (c_d2 || dis5_3)
						&& (d_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (c_d2 || dis5_3)
						&& (d_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (c_d2 || dis5_3)
						&& (c_d1 || dis5_4)
						&& (d || dis5_5))));
		// With an empty match, which leaves the range before it
		repetition_empty_ref: assert(!gate4
				|| (!(a_d4 && !dis4_0)
					|| ((b_d4 || dis4_0) && (c_d3 || dis4_1) && (d_d2 || dis4_2))
					|| ((b_d4 || dis4_0)
						&& (c_d3 || dis4_1)
						&& (c_d2 || dis4_2)
						&& (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0) && (d_d3 || dis4_1))
					|| ((b_d4 || dis4_0) && (c_d2 || dis4_2) && (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0)
						&& (c_d2 || dis4_2)
						&& (c_d1 || dis4_3)
						&& (d || dis4_4))
					|| ((b_d4 || dis4_0) && (d_d2 || dis4_2))));
		// Of a sequence of two ticks
		repetition_stride_ref: assert(!gate6
				|| (!(a_d6 && !dis6_0)
					|| ((b_d6 || dis6_0)
						&& (c_d5 || dis6_1)
						&& (d_d4 || dis6_2)
						&& (e_d3 || dis6_3))
					|| ((b_d6 || dis6_0)
						&& (c_d5 || dis6_1)
						&& (d_d4 || dis6_2)
						&& (c_d3 || dis6_3)
						&& (d_d2 || dis6_4)
						&& (e_d1 || dis6_5))
					|| ((b_d6 || dis6_0)
						&& (c_d4 || dis6_2)
						&& (d_d3 || dis6_3)
						&& (e_d2 || dis6_4))
					|| ((b_d6 || dis6_0)
						&& (c_d4 || dis6_2)
						&& (d_d3 || dis6_3)
						&& (c_d2 || dis6_4)
						&& (d_d1 || dis6_5)
						&& (e || dis6_6))));
		// Of a sequence of a range
		repetition_ranged_ref: assert(!gate6
				|| (!(a_d6 && !dis6_0)
					|| ((b_d6 || dis6_0)
						&& (c_d5 || dis6_1)
						&& (d_d5 || dis6_1)
						&& (e_d4 || dis6_2))
					|| ((b_d6 || dis6_0)
						&& (c_d5 || dis6_1)
						&& (d_d4 || dis6_2)
						&& (e_d3 || dis6_3))
					|| ((b_d6 || dis6_0)
						&& (c_d5 || dis6_1)
						&& (d_d5 || dis6_1)
						&& (c_d4 || dis6_2)
						&& (d_d4 || dis6_2)
						&& (e_d3 || dis6_3))
					|| ((b_d6 || dis6_0)
						&& (c_d5 || dis6_1)
						&& (d_d5 || dis6_1)
						&& (c_d4 || dis6_2)
						&& (d_d3 || dis6_3)
						&& (e_d2 || dis6_4))
					|| ((b_d6 || dis6_0)
						&& (c_d5 || dis6_1)
						&& (d_d4 || dis6_2)
						&& (c_d3 || dis6_3)
						&& (d_d3 || dis6_3)
						&& (e_d2 || dis6_4))
					|| ((b_d6 || dis6_0)
						&& (c_d5 || dis6_1)
						&& (d_d4 || dis6_2)
						&& (c_d3 || dis6_3)
						&& (d_d2 || dis6_4)
						&& (e_d1 || dis6_5))
					|| ((b_d6 || dis6_0)
						&& (c_d4 || dis6_2)
						&& (d_d4 || dis6_2)
						&& (e_d3 || dis6_3))
					|| ((b_d6 || dis6_0)
						&& (c_d4 || dis6_2)
						&& (d_d3 || dis6_3)
						&& (e_d2 || dis6_4))
					|| ((b_d6 || dis6_0)
						&& (c_d4 || dis6_2)
						&& (d_d4 || dis6_2)
						&& (c_d3 || dis6_3)
						&& (d_d3 || dis6_3)
						&& (e_d2 || dis6_4))
					|| ((b_d6 || dis6_0)
						&& (c_d4 || dis6_2)
						&& (d_d4 || dis6_2)
						&& (c_d3 || dis6_3)
						&& (d_d2 || dis6_4)
						&& (e_d1 || dis6_5))
					|| ((b_d6 || dis6_0)
						&& (c_d4 || dis6_2)
						&& (d_d3 || dis6_3)
						&& (c_d2 || dis6_4)
						&& (d_d2 || dis6_4)
						&& (e_d1 || dis6_5))
					|| ((b_d6 || dis6_0)
						&& (c_d4 || dis6_2)
						&& (d_d3 || dis6_3)
						&& (c_d2 || dis6_4)
						&& (d_d1 || dis6_5)
						&& (e || dis6_6))));
		// Two in a row
		repetitions_ref: assert(!gate4
				|| (!(a_d4 && !dis4_0)
					|| ((b_d4 || dis4_0) && (c_d3 || dis4_1) && (d_d2 || dis4_2))
					|| ((b_d4 || dis4_0)
						&& (c_d3 || dis4_1)
						&& (c_d2 || dis4_2)
						&& (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0)
						&& (b_d3 || dis4_1)
						&& (c_d2 || dis4_2)
						&& (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0)
						&& (b_d3 || dis4_1)
						&& (c_d2 || dis4_2)
						&& (c_d1 || dis4_3)
						&& (d || dis4_4))));
		// After a range with nothing before it
		leading_repetition_ref: assert(!gate4
				|| (!(a_d4 && !dis4_0)
					|| ((c_d3 || dis4_1) && (d_d2 || dis4_2))
					|| ((c_d3 || dis4_1) && (c_d2 || dis4_2) && (d_d1 || dis4_3))
					|| ((c_d2 || dis4_2) && (d_d1 || dis4_3))
					|| ((c_d2 || dis4_2) && (c_d1 || dis4_3) && (d || dis4_4))));
		// A concatenation after a range with nothing before it
		leading_concat_ref: assert(!gate4
				|| (!(a_d4 && !dis4_0)
					|| ((c_d3 || dis4_1) && (d_d2 || dis4_2))
					|| ((c_d3 || dis4_1) && (d_d1 || dis4_3))
					|| ((c_d2 || dis4_2) && (d_d1 || dis4_3))
					|| ((c_d2 || dis4_2) && (d || dis4_4))));
		// After a range following an empty match
		empty_before_ref: assert(!gate4
				|| (!(a_d4 && !dis4_0)
					|| ((c_d4 || dis4_0) && (d_d3 || dis4_1))
					|| ((c_d4 || dis4_0) && (c_d3 || dis4_1) && (d_d2 || dis4_2))
					|| ((c_d3 || dis4_1) && (d_d2 || dis4_2))
					|| ((c_d3 || dis4_1) && (c_d2 || dis4_2) && (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0) && (c_d3 || dis4_1) && (d_d2 || dis4_2))
					|| ((b_d4 || dis4_0)
						&& (c_d3 || dis4_1)
						&& (c_d2 || dis4_2)
						&& (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0) && (c_d2 || dis4_2) && (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0)
						&& (c_d2 || dis4_2)
						&& (c_d1 || dis4_3)
						&& (d || dis4_4))));
		// As the first element of a concatenation
		first_element_ref: assert(!gate4
				|| (!(a_d4 && !dis4_0)
					|| ((b_d4 || dis4_0) && (c_d3 || dis4_1) && (d_d2 || dis4_2))
					|| ((b_d4 || dis4_0)
						&& (c_d3 || dis4_1)
						&& (c_d2 || dis4_2)
						&& (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0) && (c_d2 || dis4_2) && (d_d1 || dis4_3))
					|| ((b_d4 || dis4_0)
						&& (c_d2 || dis4_2)
						&& (c_d1 || dis4_3)
						&& (d || dis4_4))));
		// First_match of a range
		first_match_ref: assert(!gate5
				|| (!(a_d5 && !dis5_0)
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (d_d3 || dis5_2)
						&& (e_d2 || dis5_3))
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (d_d2 || dis5_3)
						&& !((c_d4 || dis5_1) && (d_d3 || dis5_2))
						&& (e_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (d_d2 || dis5_3)
						&& (e_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (d_d1 || dis5_4)
						&& !((c_d3 || dis5_2) && (d_d2 || dis5_3))
						&& (e || dis5_5))));
		// First_match of a leading range
		first_match_leading_ref: assert(!gate5
				|| (!(a_d5 && !dis5_0)
					|| ((b_d5 || dis5_0) && (d_d3 || dis5_2) && (e_d2 || dis5_3))
					|| ((b_d5 || dis5_0)
						&& (d_d2 || dis5_3)
						&& !(d_d3 || dis5_2)
						&& (e_d1 || dis5_4))
					|| ((b_d5 || dis5_0) && (d_d2 || dis5_3) && (e_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (d_d1 || dis5_4)
						&& !(d_d2 || dis5_3)
						&& (e || dis5_5))));
		// First_match of a range after fixed delays
		first_match_chain_ref: assert(!gate5
				|| (!(a_d5 && !dis5_0)
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (d_d3 || dis5_2)
						&& (e_d3 || dis5_2))
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (d_d3 || dis5_2)
						&& (e_d2 || dis5_3)
						&& !((c_d4 || dis5_1) && (d_d3 || dis5_2) && (e_d3 || dis5_2)))
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (d_d3 || dis5_2)
						&& (e_d1 || dis5_4)
						&& !(((c_d4 || dis5_1) && (d_d3 || dis5_2) && (e_d3 || dis5_2))
							|| ((c_d4 || dis5_1) && (d_d3 || dis5_2) && (e_d2 || dis5_3))))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (d_d2 || dis5_3)
						&& (e_d2 || dis5_3))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (d_d2 || dis5_3)
						&& (e_d1 || dis5_4)
						&& !((c_d3 || dis5_2) && (d_d2 || dis5_3) && (e_d2 || dis5_3)))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (d_d2 || dis5_3)
						&& (e || dis5_5)
						&& !(((c_d3 || dis5_2) && (d_d2 || dis5_3) && (e_d2 || dis5_3))
							|| ((c_d3 || dis5_2) && (d_d2 || dis5_3) && (e_d1 || dis5_4))))));
		// Throughout of a range
		throughout_ref: assert(!gate5
				|| (!(a_d5 && !dis5_0)
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (d_d3 || dis5_2)
						&& (e_d4 || dis5_1)
						&& (e_d3 || dis5_2)
						&& (a_d2 || dis5_3))
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (d_d2 || dis5_3)
						&& (e_d4 || dis5_1)
						&& (e_d3 || dis5_2)
						&& (e_d2 || dis5_3)
						&& (a_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (d_d2 || dis5_3)
						&& (e_d3 || dis5_2)
						&& (e_d2 || dis5_3)
						&& (a_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (d_d1 || dis5_4)
						&& (e_d3 || dis5_2)
						&& (e_d2 || dis5_3)
						&& (e_d1 || dis5_4)
						&& (a || dis5_5))));
		// Throughout of a ranged repetition
		throughout_repetition_ref: assert(!gate5
				|| (!(a_d5 && !dis5_0)
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (e_d4 || dis5_1)
						&& (d_d3 || dis5_2))
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (c_d3 || dis5_2)
						&& (e_d4 || dis5_1)
						&& (e_d3 || dis5_2)
						&& (d_d2 || dis5_3))
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (c_d3 || dis5_2)
						&& (c_d2 || dis5_3)
						&& (e_d4 || dis5_1)
						&& (e_d3 || dis5_2)
						&& (e_d2 || dis5_3)
						&& (d_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (e_d3 || dis5_2)
						&& (d_d2 || dis5_3))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (c_d2 || dis5_3)
						&& (e_d3 || dis5_2)
						&& (e_d2 || dis5_3)
						&& (d_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (c_d2 || dis5_3)
						&& (c_d1 || dis5_4)
						&& (e_d3 || dis5_2)
						&& (e_d2 || dis5_3)
						&& (e_d1 || dis5_4)
						&& (d || dis5_5))));
		// Throughout of first_match
		throughout_first_match_ref: assert(!gate4
				|| (!(a_d4 && !dis4_0)
					|| ((b_d4 || dis4_0)
						&& (c_d3 || dis4_1)
						&& (d_d2 || dis4_2)
						&& (e_d3 || dis4_1)
						&& (e_d2 || dis4_2))
					|| ((b_d4 || dis4_0)
						&& (c_d3 || dis4_1)
						&& (d_d1 || dis4_3)
						&& !((c_d3 || dis4_1) && (d_d2 || dis4_2))
						&& (e_d3 || dis4_1)
						&& (e_d2 || dis4_2)
						&& (e_d1 || dis4_3))
					|| ((b_d4 || dis4_0)
						&& (c_d2 || dis4_2)
						&& (d_d1 || dis4_3)
						&& (e_d2 || dis4_2)
						&& (e_d1 || dis4_3))
					|| ((b_d4 || dis4_0)
						&& (c_d2 || dis4_2)
						&& (d || dis4_4)
						&& !((c_d2 || dis4_2) && (d_d1 || dis4_3))
						&& (e_d2 || dis4_2)
						&& (e_d1 || dis4_3)
						&& (e || dis4_4))));
		// Throughout of throughout
		throughout_nested_ref: assert(!gate4
				|| (!(a_d4 && !dis4_0)
					|| ((b_d4 || dis4_0)
						&& (c_d3 || dis4_1)
						&& (d_d2 || dis4_2)
						&& (a_d3 || dis4_1)
						&& (a_d2 || dis4_2)
						&& (e_d3 || dis4_1)
						&& (e_d2 || dis4_2))
					|| ((b_d4 || dis4_0)
						&& (c_d3 || dis4_1)
						&& (d_d1 || dis4_3)
						&& (a_d3 || dis4_1)
						&& (a_d2 || dis4_2)
						&& (a_d1 || dis4_3)
						&& (e_d3 || dis4_1)
						&& (e_d2 || dis4_2)
						&& (e_d1 || dis4_3))
					|| ((b_d4 || dis4_0)
						&& (c_d2 || dis4_2)
						&& (d_d1 || dis4_3)
						&& (a_d2 || dis4_2)
						&& (a_d1 || dis4_3)
						&& (e_d2 || dis4_2)
						&& (e_d1 || dis4_3))
					|| ((b_d4 || dis4_0)
						&& (c_d2 || dis4_2)
						&& (d || dis4_4)
						&& (a_d2 || dis4_2)
						&& (a_d1 || dis4_3)
						&& (a || dis4_4)
						&& (e_d2 || dis4_2)
						&& (e_d1 || dis4_3)
						&& (e || dis4_4))));
		// Or of ranges
		or_ref: assert(!gate5
				|| (!(a_d5 && !dis5_0)
					|| ((b_d5 || dis5_0) && (c_d4 || dis5_1) && (a_d3 || dis5_2))
					|| ((b_d5 || dis5_0)
						&& (c_d4 || dis5_1)
						&& (c_d3 || dis5_2)
						&& (a_d2 || dis5_3))
					|| ((b_d5 || dis5_0)
						&& (d_d4 || dis5_1)
						&& (e_d3 || dis5_2)
						&& (a_d2 || dis5_3))
					|| ((b_d5 || dis5_0)
						&& (d_d4 || dis5_1)
						&& (e_d2 || dis5_3)
						&& (a_d1 || dis5_4))
					|| ((b_d5 || dis5_0) && (c_d3 || dis5_2) && (a_d2 || dis5_3))
					|| ((b_d5 || dis5_0)
						&& (c_d3 || dis5_2)
						&& (c_d2 || dis5_3)
						&& (a_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (d_d3 || dis5_2)
						&& (e_d2 || dis5_3)
						&& (a_d1 || dis5_4))
					|| ((b_d5 || dis5_0)
						&& (d_d3 || dis5_2)
						&& (e_d1 || dis5_4)
						&& (a || dis5_5))));
		// In an antecedent
		antecedent_ref: assert(!gate3
				|| ((!(a_d3 && !dis3_0 && b_d2 && !dis3_1) || c_d2 || dis3_1)
					&& (!(a_d3 && !dis3_0 && b_d2 && !dis3_1 && b_d1 && !dis3_2)
						|| c_d1
						|| dis3_2)
					&& (!(a_d3 && !dis3_0 && b_d1 && !dis3_2) || c_d1 || dis3_2)
					&& (!(a_d3 && !dis3_0 && b_d1 && !dis3_2 && b && !dis3_3) || c || dis3_3)));
		// Throughout in an antecedent
		antecedent_throughout_ref: assert(!gate5
				|| ((!(a_d5
							&& !dis5_0
							&& b_d4
							&& !dis5_1
							&& c_d3
							&& !dis5_2
							&& e_d4
							&& !dis5_1
							&& e_d3
							&& !dis5_2)
						|| d_d2
						|| dis5_3)
					&& (!(a_d5
							&& !dis5_0
							&& b_d4
							&& !dis5_1
							&& c_d2
							&& !dis5_3
							&& e_d4
							&& !dis5_1
							&& e_d3
							&& !dis5_2
							&& e_d2
							&& !dis5_3)
						|| d_d1
						|| dis5_4)
					&& (!(a_d5
							&& !dis5_0
							&& b_d3
							&& !dis5_2
							&& c_d2
							&& !dis5_3
							&& e_d3
							&& !dis5_2
							&& e_d2
							&& !dis5_3)
						|| d_d1
						|| dis5_4)
					&& (!(a_d5
							&& !dis5_0
							&& b_d3
							&& !dis5_2
							&& c_d1
							&& !dis5_4
							&& e_d3
							&& !dis5_2
							&& e_d2
							&& !dis5_3
							&& e_d1
							&& !dis5_4)
						|| d
						|| dis5_5)));
		// A cover
		cover_repetition_ref: cover(gate4
				&& ((a_d4 && !dis4_0 && b_d3 && !dis4_1 && c_d2 && !dis4_2)
					|| (a_d4
						&& !dis4_0
						&& b_d3
						&& !dis4_1
						&& b_d2
						&& !dis4_2
						&& c_d1
						&& !dis4_3)
					|| (a_d4 && !dis4_0 && b_d2 && !dis4_2 && c_d1 && !dis4_3)
					|| (a_d4 && !dis4_0 && b_d2 && !dis4_2 && b_d1 && !dis4_3 && c && !dis4_4)));

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
		r_d1 <= r;
		r_d2 <= r_d1;
		r_d3 <= r_d2;
		r_d4 <= r_d3;
		r_d5 <= r_d4;
		r_d6 <= r_d5;
	end

	repetition_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] c[*1:2] ##1 d);
	repetition_fewest_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] c[*2:3] ##1 d);
	repetition_empty_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] c[*0:2] ##1 d);
	repetition_stride_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] (c ##1 d)[*1:2] ##1 e);
	repetition_ranged_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] (c ##[0:1] d)[*1:2] ##1 e);
	repetitions_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b[*1:2] ##1 c[*1:2] ##1 d);
	leading_repetition_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> ##[1:2] c[*1:2] ##1 d);
	leading_concat_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> ##[1:2] (c ##[1:2] d));
	empty_before_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b[*0:1] ##[1:2] c[*1:2] ##1 d);
	first_element_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] (c[*1:2] ##1 d));
	first_match_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] first_match(c ##[1:2] d) ##1 e);
	first_match_leading_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] first_match(##[1:2] d) ##1 e);
	first_match_chain_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] first_match(c ##1 d ##[0:2] e));
	throughout_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] (e throughout (c ##[1:2] d)) ##1 a);
	throughout_repetition_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] (e throughout c[*1:3]) ##1 d);
	throughout_first_match_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] (e throughout first_match(c ##[1:2] d)));
	throughout_nested_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] (e throughout (a throughout (c ##[1:2] d))));
	or_test: assert property(@(posedge clk_i) disable iff (r)
		a |-> b ##[1:2] (c[*1:2] or (d ##[1:2] e)) ##1 a);
	antecedent_test: assert property(@(posedge clk_i) disable iff (r) a ##[1:2] b[*1:2] |-> c);
	antecedent_throughout_test: assert property(@(posedge clk_i) disable iff (r)
		a ##[1:2] (e throughout (b ##[1:2] c)) |=> d);
	cover_repetition_test: cover property(@(posedge clk_i) disable iff (r) a ##[1:2] b[*1:2] ##1 c);
endmodule
