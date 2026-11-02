// The rules of zero_delay_empty.sv in the monitor that matches an unbounded antecedent:
// a leading `##0` keeps only the non-empty matches of what follows, while a bare first
// element keeps them all
module zero_delay_empty_monitor(input clk_i, input logic a, input logic b, input logic c);
	reg gate1 = 0;
	reg gate2 = 0;
	reg a_d1;
	reg a_d2;
	reg b_d1;

	always @(posedge clk_i) begin
		prefix_ref: assert(!gate1 || !(a_d1 && b) || c);
		only_empty_ref: assert(1);
		range_ref: assert(!gate1 || !b || c);
		nonoverlap_ref: assert(!gate2 || !(a_d2 && b_d1) || c);
		bare_first_ref: assert(!b || c);
		delayed_ref: assert(!gate1 || !b || c);

		gate1 <= 1;
		gate2 <= gate1;
		a_d1 <= a;
		a_d2 <= a_d1;
		b_d1 <= b;
	end

	prefix_test: assert property(@(posedge clk_i) ((##0 a[*0:$]) ##1 b) |-> c);
	only_empty_test: assert property(@(posedge clk_i) ((##0 1'b0[*0:$]) ##1 b) |-> c);
	range_test: assert property(@(posedge clk_i) ((##[0:1] a[*0:$]) ##1 b) |-> c);
	nonoverlap_test: assert property(@(posedge clk_i) ((##0 a[*0:$]) ##1 b) |=> c);
	bare_first_test: assert property(@(posedge clk_i) (a[*0:$] ##1 b) |-> c);
	delayed_test: assert property(@(posedge clk_i) (##1 a[*0:$] ##1 b) |-> c);
endmodule
