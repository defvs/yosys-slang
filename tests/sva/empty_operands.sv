module empty_operands(input clk_i, input logic a, input logic b, input logic c);
	reg gate = 0;
	reg a_d1;
	reg b_d1;
	reg c_d1;

	always @(posedge clk_i) begin
		and_ref: assert(b);
		intersect_ref: assert(a && b);
		throughout_ref: assert(!gate || c_d1 || (a_d1 && b_d1 && c));
		first_match_ref: assert(b);

		gate <= 1;
		a_d1 <= a;
		b_d1 <= b;
		c_d1 <= c;
	end

	and_test: assert property(@(posedge clk_i) a[*0:1] and b);
	intersect_test: assert property(@(posedge clk_i) a[*0:1] intersect b);
	throughout_test: assert property(@(posedge clk_i) (a throughout b[*0:1]) ##1 c);
	first_match_test: assert property(@(posedge clk_i) first_match(a[*0:1]) ##1 b);
endmodule
