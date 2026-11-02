// A procedural condition decides if an attempt is queued when the statement is
// reached, attempts already in flight don't depend on it (IEEE Std 1800-2017
// section 16.14.6). The references delay the condition along with the property.
module procedural_guards(input logic clk_i, input logic c, input logic e,
						 input logic [1:0] m, input logic a, input logic b);
	reg c_d1 = 0, c_d2 = 0;
	reg not_c_d1 = 0;
	reg c_e_d1 = 0;
	reg m_one_d1 = 0, m_two_d1 = 0, m_other_d1 = 0;
	reg not_a_d1 = 0;
	reg a_d1 = 0, a_d2 = 0;
	reg b_d1 = 0;
	reg e_d1 = 0;
	reg [1:0] n = 0;
	logic v;

	always @(posedge clk_i) begin
		// The one attempt starts at n == 1 and fails at n == 2, with the condition low by then
		dropped_ref: assert(n != 2);
		if (n == 1)
			dropped_test: assert property ((n == 1) |=> (n == 3));

		// The one attempt starts at n == 2, the antecedent's match at n == 1 doesn't count
		stale_ref: assert(1);
		if (n == 2)
			stale_test: assert property ((n == 1) |=> (n == 3));

		if_sequence_ref: assert(!c_d1 || (a_d1 && b));
		if (c)
			if_sequence_test: assert property (a ##1 b);

		if_implication_ref: assert(!c_d1 || !a_d1 || b);
		if (c)
			if_implication_test: assert property (a |=> b);

		then_ref: assert(!c_d1 || !b_d1 || a);
		else_ref: assert(!not_c_d1 || !a_d1 || b);
		if (c)
			then_test: assert property (b |=> a);
		else
			else_test: assert property (a |=> b);

		nested_ref: assert(!c_e_d1 || !a_d1 || b);
		if (c) begin
			if (e)
				nested_test: assert property (a |=> b);
		end

		case_one_ref: assert(!m_one_d1 || !a_d1 || b);
		case_two_ref: assert(!m_two_d1 || !b_d1 || a);
		case_other_ref: assert(!m_other_d1 || (a_d1 && b));
		case (m)
			2'd1: begin
				case_one_test: assert property (a |=> b);
			end
			2'd2: begin
				case_two_test: assert property (b |=> a);
			end
			default: begin
				case_other_test: assert property (a ##1 b);
			end
		endcase

		two_ticks_ref: assert(!c_d2 || (a_d2 && b));
		if (c)
			two_ticks_test: assert property (a ##2 b);

		// Reported at the end of the longest path
		ranged_ref: assert(!c_d2 || (a_d2 && (b_d1 || b)));
		if (c)
			ranged_test: assert property (a ##[1:2] b);

		disabled_ref: assert(!c_d1 || e_d1 || e || !a_d1 || b);
		if (c)
			disabled_test: assert property (disable iff (e) a |=> b);

		// The condition is plain procedural code, unlike the sampled property operands
		v = !a;
		blocking_ref: assert(!not_a_d1 || !b_d1 || a);
		if (v)
			blocking_test: assert property (b |=> a);

		single_ref: assert(!c || a);
		if (c)
			single_test: assert property (a);

		// No possible match, so every attempt fails on its first tick
		no_match_ref: assert(!c);
		if (c)
			no_match_test: assert property ((a ##1 b) intersect (b ##2 a));

		c_d1 <= c;
		c_d2 <= c_d1;
		not_c_d1 <= !c;
		c_e_d1 <= c && e;
		m_one_d1 <= m == 2'd1;
		m_two_d1 <= m == 2'd2;
		m_other_d1 <= m == 2'd0 || m == 2'd3;
		not_a_d1 <= !a;
		a_d1 <= a;
		a_d2 <= a_d1;
		b_d1 <= b;
		e_d1 <= e;
		if (n != 3)
			n <= n + 1;
	end

	reg iff_c_d1 = 0, iff_a_d1 = 0;

	// The delay to the report goes by the ticks of the process
	always @(posedge clk_i iff e) begin
		iff_ref: assert(!iff_c_d1 || !iff_a_d1 || b);
		if (c)
			iff_test: assert property (a |=> b);

		iff_c_d1 <= c;
		iff_a_d1 <= a;
	end
endmodule
