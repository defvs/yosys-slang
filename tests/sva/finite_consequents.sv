// Finite property operators the sequence paths have no lowering for, as the consequent
// of an implication or followed-by and as an operand of property `and`. The operator
// around them takes them apart, starting them on the match of its antecedent. strong()
// and weak() of a bounded sequence are that sequence used as a property, where the two
// agree, so the paths take them.
module finite_consequents(input clk_i, input logic g, input logic x, input logic y,
						  input logic r, input logic a, input logic b, input logic c,
						  input logic d, input logic [1:0] v);
	reg x_d1 = 0, a_d1 = 0, b_d1 = 0, c_d1 = 0, r_d1 = 0;

	always @(posedge clk_i) begin
		// The branches fail on the tick each of them ends on
		if_lengths_ref: assert(!(x && a && !b) && !(x_d1 && !a_d1 && !(c_d1 && d)));
		// From the tick `r` holds on the letters are T
		accept_ref: assert(!x_d1 || r_d1 || (a_d1 && (b || r)));
		iff_ref: assert(!x || a == b);
		and_ref: assert((!x || a) && b);
		and_lengths_ref: assert(c && !(x_d1 && !(a_d1 && b)));
		// `x` must match, then `if (a) b` holds from it
		followed_ref: assert(x && (!a || b));
		followed_next_ref: assert(x && !(x_d1 && a && !b));
		x_d1 <= x;
		a_d1 <= a;
		b_d1 <= b;
		c_d1 <= c;
		r_d1 <= r;
	end

	always @(posedge clk_i) begin
		if (g) begin
			guard_ref: assert property (x && a |-> b ##1 c);
			guard_test: assert property (x |-> if (a) b ##1 c);
		end
	end

	strong_ref: assert property (@(posedge clk_i) x |-> a ##1 b);
	strong_test: assert property (@(posedge clk_i) x |-> strong(a ##1 b));

	weak_ref: assert property (@(posedge clk_i) x |=> a ##[0:1] b);
	weak_test: assert property (@(posedge clk_i) x |=> weak(a ##[0:1] b));

	not_strong_ref: assert property (@(posedge clk_i) not (a ##1 b));
	not_strong_test: assert property (@(posedge clk_i) not strong(a ##1 b));

	or_strong_ref: assert property (@(posedge clk_i) (a ##1 b) or (c ##2 d));
	or_strong_test: assert property (@(posedge clk_i) strong(a ##1 b) or weak(c ##2 d));

	if_ref: assert property (@(posedge clk_i) x && a |-> b ##1 c);
	if_test: assert property (@(posedge clk_i) x |-> if (a) b ##1 c);

	if_else_ref: assert property (@(posedge clk_i) x |-> (a ? b : d) ##1 c);
	if_else_test: assert property (@(posedge clk_i) x |-> if (a) b ##1 c else d ##1 c);

	if_lengths_test: assert property (@(posedge clk_i) x |-> if (a) b else c ##1 d);

	case_ref: assert property (@(posedge clk_i)
		x |=> (v == 2'd0 ? a : (v == 2'd1 || v == 2'd2) ? c : d) ##1 b);
	case_test: assert property (@(posedge clk_i)
		x |=> case (v) 2'd0: a ##1 b; 2'd1, 2'd2: c ##1 b; default: d ##1 b; endcase);

	accept_test: assert property (@(posedge clk_i) x |-> accept_on (r) a ##1 b);

	iff_test: assert property (@(posedge clk_i) x |-> (a iff b));

	implies_ref: assert property (@(posedge clk_i) x && a |-> b ##1 c);
	implies_test: assert property (@(posedge clk_i) x |-> (a implies b ##1 c));

	and_test: assert property (@(posedge clk_i) (if (x) a) and b);
	and_lengths_test: assert property (@(posedge clk_i) (if (x) a ##1 b) and c);

	followed_test: assert property (@(posedge clk_i) x #-# (if (a) b));
	followed_next_test: assert property (@(posedge clk_i) x #=# (if (a) b));

	sequence_antecedent_ref: assert property (@(posedge clk_i) x ##1 y ##1 a |-> b);
	sequence_antecedent_test: assert property (@(posedge clk_i) x ##1 y |=> if (a) b);

	disable_ref: assert property (@(posedge clk_i) disable iff (r) x && a |-> b ##1 c);
	disable_test: assert property (@(posedge clk_i) disable iff (r) x |-> if (a) b ##1 c);

	assume_ref: assume property (@(posedge clk_i) x && a |-> b ##1 c);
	assume_test: assume property (@(posedge clk_i) x |-> if (a) b ##1 c);

	cover_ref: cover property (@(posedge clk_i) x |-> a ##1 b);
	cover_test: cover property (@(posedge clk_i) x |-> strong(a ##1 b));
endmodule
