module repetition_edges(input clk_i, input logic x, input logic y, input logic z);
	reg gate = 0;
	reg gate2 = 0;
	reg gate3 = 0;
	reg x_d1;
	reg x_d2;
	reg y_d1;
	reg z_d1;

	always @(posedge clk_i) begin
		repeat0_ref: assert(!gate || y);
		repeat01_ref: assert(!gate || y);
		repeat11_ref: assert(!gate || (x_d1 && y));
		right_empty1_ref: assert(x);
		right_empty2_ref: assert(!gate || x_d1);
		empty_delay0_antecedent_ref: assert(1);
		empty_empty_ref: assert(!gate2 || z);
		leading_delayed_empty_ref: assert(!gate2 || z);
		optional_delay2_ref: assert(!gate2 || (y_d1 || (x_d2 && y)));
		leading_range_empty_ref: assert(!gate2 || (z_d1 || z));
		empty_empty_range_ref: assert(!gate2 || (z_d1 || z));
		right_empty_range_ref: assert(!gate2 || (x_d2 && (z_d1 || z)));
		leading_delayed_empty_range_ref: assert(!gate3 || (z_d1 || z));

		gate <= 1;
		gate2 <= gate;
		gate3 <= gate2;
		x_d1 <= x;
		x_d2 <= x_d1;
		y_d1 <= y;
		z_d1 <= z;
	end

	repeat0_test: assert property(@(posedge clk_i) x[*0] ##1 y);
	repeat01_test: assert property(@(posedge clk_i) x[*0:1] ##1 y);
	repeat11_test: assert property(@(posedge clk_i) x[*1:1] ##1 y);
	right_empty1_test: assert property(@(posedge clk_i) x ##1 y[*0]);
	right_empty2_test: assert property(@(posedge clk_i) x ##2 y[*0]);
	empty_delay0_antecedent_test: assert property(@(posedge clk_i) (x[*0] ##0 y) |-> 0);
	empty_empty_test: assert property(@(posedge clk_i) x[*0] ##2 y[*0] ##1 z);
	leading_delayed_empty_test: assert property(@(posedge clk_i) ##2 x[*0] ##1 z);
	optional_delay2_test: assert property(@(posedge clk_i) x[*0:1] ##2 y);
	leading_range_empty_test: assert property(@(posedge clk_i) ##[1:2] x[*0] ##1 z);
	empty_empty_range_test: assert property(@(posedge clk_i) x[*0] ##[1:2] y[*0] ##1 z);
	right_empty_range_test: assert property(@(posedge clk_i) x ##[1:2] y[*0] ##1 z);
	leading_delayed_empty_range_test: assert property(@(posedge clk_i) ##2 x[*0] ##[1:2] z);
endmodule
