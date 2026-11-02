// A bounded sequence settles all its matches once its longest path is over, so
// strong() and weak() of it are the sequence used as a property
module strong_weak(input clk_i, input logic a, input logic b, input logic c, input logic x);
	reg gate1 = 0;
	reg gate2 = 0;
	reg a_d1;
	reg b_d1;
	reg c_d1;
	reg x_d1;
	reg x_d2;

	always @(posedge clk_i) begin
		strong_ref: assert(!gate1 || (a_d1 && b));
		weak_ref: assert(!gate1 || (a_d1 && b));
		strong_consequent_ref: assert(!gate1 || !x_d1 || (a_d1 && b));
		weak_range_consequent_ref: assert(!gate2 || !x_d2 || (a_d1 && (b_d1 || b)));
		range_ref: assert(!gate1 || (a_d1 && (b_d1 || b)));
		property_and_ref: assert(!gate1 || (c_d1 && a_d1 && b));
		cover_ref: cover(gate1 && a_d1 && b);

		gate1 <= 1;
		gate2 <= gate1;
		a_d1 <= a;
		b_d1 <= b;
		c_d1 <= c;
		x_d1 <= x;
		x_d2 <= x_d1;
	end

	strong_test: assert property(@(posedge clk_i) strong(a ##1 b));
	weak_test: assert property(@(posedge clk_i) weak(a ##1 b));
	strong_consequent_test: assert property(@(posedge clk_i) x |-> strong(a ##1 b));
	weak_range_consequent_test: assert property(@(posedge clk_i) x |=> weak(a ##[0:1] b));
	range_test: assert property(@(posedge clk_i) strong(a ##[0:1] b));
	property_and_test: assert property(@(posedge clk_i) strong(a ##1 b) and c);
	cover_test: cover property(@(posedge clk_i) strong(a ##1 b));
endmodule
