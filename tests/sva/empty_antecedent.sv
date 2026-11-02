// The rules for antecedents differ: R |-> P has an empty match of R start nothing
// and R |=> P is (R ##1 `true) |-> P, where (empty ##1 `true) is `true
module empty_antecedent(input clk_i, input logic a, input logic b);
	reg gate1 = 0;
	reg a_d1;
	reg b_d1;

	always @(posedge clk_i) begin
		overlap_ref: assert(1);
		overlap_other_match_ref: assert(!a || b);
		nonoverlap_ref: assert(!gate1 || a_d1);
		nonoverlap_other_match_ref: assert(!gate1 || (a_d1 && (!b_d1 || a)));

		gate1 <= 1;
		a_d1 <= a;
		b_d1 <= b;
	end

	overlap_test: assert property(@(posedge clk_i) 1'b0[*0:1] |-> a);
	overlap_other_match_test: assert property(@(posedge clk_i) (1'b0[*0:1] or a) |-> b);
	nonoverlap_test: assert property(@(posedge clk_i) 1'b0[*0:1] |=> a);
	nonoverlap_other_match_test: assert property(@(posedge clk_i) b[*0:1] |=> a);
endmodule
