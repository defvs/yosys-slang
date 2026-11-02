// `##N seq` is `1'b1 ##N seq` and (seq ##0 empty) has no match, so a leading `##0`
// keeps only the non-empty matches of what follows. A bare first element keeps them all.
module zero_delay_empty(input clk_i, input logic a, input logic b);
	reg gate1 = 0;
	reg gate2 = 0;
	reg a_d1;
	reg a_d2;
	reg b_d1;

	always @(posedge clk_i) begin
		prefix_ref: assert(!gate1 || (a_d1 && b));
		grouped_ref: assert(!gate1 || (a_d1 && b));
		only_empty_ref: assert(0);
		range_ref: assert(!gate2 || (a_d2 && b_d1) || (a_d1 && b) || b_d1);
		antecedent_ref: assert(!gate1 || !a_d1 || b);
		antecedent_only_empty_ref: assert(1);
		antecedent_range_ref: assert(!gate2 || (b_d1 && (!a_d1 || b)));
		bare_first_ref: assert(!gate1 || (a_d1 && b) || b_d1);
		delayed_ref: assert(!gate2 || (a_d1 && b) || b_d1);

		gate1 <= 1;
		gate2 <= gate1;
		a_d1 <= a;
		a_d2 <= a_d1;
		b_d1 <= b;
	end

	prefix_test: assert property(@(posedge clk_i) ##0 a[*0:1] ##1 b);
	grouped_test: assert property(@(posedge clk_i) (##0 a[*0:1]) ##1 b);
	only_empty_test: assert property(@(posedge clk_i) (##0 a[*0]) ##1 b);
	range_test: assert property(@(posedge clk_i) (##[0:1] a[*0:1]) ##1 b);
	antecedent_test: assert property(@(posedge clk_i) (##0 a[*0:1]) |=> b);
	antecedent_only_empty_test: assert property(@(posedge clk_i) (##0 a[*0]) |=> b);
	antecedent_range_test: assert property(@(posedge clk_i) (##[0:1] a[*0:1]) |=> b);
	bare_first_test: assert property(@(posedge clk_i) a[*0:1] ##1 b);
	delayed_test: assert property(@(posedge clk_i) ##1 a[*0:1] ##1 b);
endmodule
