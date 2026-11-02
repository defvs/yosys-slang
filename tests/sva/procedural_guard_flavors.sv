// The condition around an assume or a cover works as it does for an assert: it
// decides if an attempt is queued when the statement is reached (IEEE Std 1800-2017
// section 16.14.6). The references delay the condition along with the property.
module procedural_guard_flavors(input logic clk_i, input logic c, input logic [1:0] m,
								input logic a, input logic b);
	reg c_d1 = 0, c_d2 = 0;
	reg not_c_d1 = 0;
	reg m_one_d1 = 0, m_other_d1 = 0;
	reg a_d1 = 0, a_d2 = 0;
	reg b_d1 = 0;
	reg [1:0] n = 0;

	always @(posedge clk_i) begin
		assume_dropped_ref: assume(n != 2 || b);
		if (n == 1)
			assume_dropped_test: assume property ((n == 1) |=> b);

		assume_stale_ref: assume(1);
		if (n == 2)
			assume_stale_test: assume property ((n == 1) |=> b);

		assume_if_ref: assume(!c_d1 || (a_d1 && b));
		if (c)
			assume_if_test: assume property (a ##1 b);

		assume_then_ref: assume(!c_d1 || !b_d1 || a);
		assume_else_ref: assume(!not_c_d1 || !a_d1 || b);
		if (c)
			assume_then_test: assume property (b |=> a);
		else
			assume_else_test: assume property (a |=> b);

		assume_one_ref: assume(!m_one_d1 || !a_d1 || b);
		assume_other_ref: assume(!m_other_d1 || !b_d1 || a);
		case (m)
			2'd1: begin
				assume_one_test: assume property (a |=> b);
			end
			default: begin
				assume_other_test: assume property (b |=> a);
			end
		endcase

		assume_single_ref: assume(!c || a);
		if (c)
			assume_single_test: assume property (a);

		// The one attempt starts at n == 1 and is hit at n == 2, with the condition low by then
		cover_dropped_ref: cover(n == 2);
		if (n == 1)
			cover_dropped_test: cover property ((n == 1) ##1 (n == 2));

		cover_stale_ref: cover(0);
		if (n == 2)
			cover_stale_test: cover property ((n == 1) ##1 (n == 2));

		cover_if_ref: cover(c_d1 && a_d1 && b);
		if (c)
			cover_if_test: cover property (a ##1 b);

		cover_then_ref: cover(c_d1 && b_d1 && a);
		cover_else_ref: cover(not_c_d1 && a_d1 && b);
		if (c)
			cover_then_test: cover property (b ##1 a);
		else
			cover_else_test: cover property (a ##1 b);

		cover_one_ref: cover(m_one_d1 && a_d1 && b);
		cover_other_ref: cover(m_other_d1 && b_d1 && a);
		case (m)
			2'd1: begin
				cover_one_test: cover property (a ##1 b);
			end
			default: begin
				cover_other_test: cover property (b ##1 a);
			end
		endcase

		// Reported at the end of the longest path
		cover_ranged_ref: cover(c_d2 && a_d2 && (b_d1 || b));
		if (c)
			cover_ranged_test: cover property (a ##[1:2] b);

		cover_single_ref: cover(c && a);
		if (c)
			cover_single_test: cover property (a);

		c_d1 <= c;
		c_d2 <= c_d1;
		not_c_d1 <= !c;
		m_one_d1 <= m == 2'd1;
		m_other_d1 <= m != 2'd1;
		a_d1 <= a;
		a_d2 <= a_d1;
		b_d1 <= b;
		if (n != 3)
			n <= n + 1;
	end
endmodule
