module finite_first_match(input clk_i, input logic x, input logic y);
	reg gate1 = 0;
	reg gate2 = 0;
	reg gate3 = 0;
	reg x_d1;
	reg x_d2;
	reg x_d3;
	reg y_d1;
	reg y_d2;

	always @(posedge clk_i) begin
		first_match_ref: assert(!gate3 || (x_d3 && (y_d2 || y_d1 || y)));

		gate1 <= 1;
		gate2 <= gate1;
		gate3 <= gate2;
		x_d1 <= x;
		x_d2 <= x_d1;
		x_d3 <= x_d2;
		y_d1 <= y;
		y_d2 <= y_d1;
	end

	first_match_test: assert property(@(posedge clk_i) first_match(x ##[1:3] y));
endmodule
