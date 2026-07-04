module pr_sequence(input clk_i, input logic x, input logic y);
	reg gate1 = 0;
	reg gate2 = 0;
	reg x_d1;
	reg x_d2;
	reg y_d1;

	always @(posedge clk_i) begin
		delay_ref: assert(!gate1 || (x_d1 && y));
		range_ref: assert(!gate2 || (x_d2 && (y_d1 || y)));

		gate1 <= 1;
		gate2 <= gate1;
		x_d1 <= x;
		x_d2 <= x_d1;
		y_d1 <= y;
	end

	delay_test: assert property(@(posedge clk_i) x ##1 y);
	range_test: assert property(@(posedge clk_i) x ##[1:2] y);
endmodule
