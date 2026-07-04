module ranged_delay_edges(input clk_i, input logic x, input logic y, input logic z);
	reg gate1 = 0;
	reg gate2 = 0;
	reg x_d1;
	reg x_d2;
	reg y_d1;

	always @(posedge clk_i) begin
		delay0_ref: assert(x && y);
		range01_ref: assert(!gate1 || (x_d1 && (y_d1 || y)));
		range22_ref: assert(!gate2 || (x_d2 && z));

		gate1 <= 1;
		gate2 <= gate1;
		x_d1 <= x;
		x_d2 <= x_d1;
		y_d1 <= y;
	end

	delay0_test: assert property(@(posedge clk_i) x ##0 y);
	range01_test: assert property(@(posedge clk_i) x ##[0:1] y);
	range22_test: assert property(@(posedge clk_i) x ##[2:2] z);
endmodule
