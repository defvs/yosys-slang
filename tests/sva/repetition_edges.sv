module repetition_edges(input clk_i, input logic x, input logic y, input logic z);
	reg gate = 0;
	reg gate2 = 0;
	reg x_d1;

	always @(posedge clk_i) begin
		repeat0_ref: assert(!gate || y);
		repeat01_ref: assert(!gate || y);
		repeat11_ref: assert(!gate || (x_d1 && y));
		right_empty1_ref: assert(x);
		right_empty2_ref: assert(!gate || x_d1);
		empty_delay0_antecedent_ref: assert(1);
		empty_empty_ref: assert(!gate2 || z);
		leading_delayed_empty_ref: assert(!gate2 || z);

		gate <= 1;
		gate2 <= gate;
		x_d1 <= x;
	end

	repeat0_test: assert property(@(posedge clk_i) x[*0] ##1 y);
	repeat01_test: assert property(@(posedge clk_i) x[*0:1] ##1 y);
	repeat11_test: assert property(@(posedge clk_i) x[*1:1] ##1 y);
	right_empty1_test: assert property(@(posedge clk_i) x ##1 y[*0]);
	right_empty2_test: assert property(@(posedge clk_i) x ##2 y[*0]);
	empty_delay0_antecedent_test: assert property(@(posedge clk_i) (x[*0] ##0 y) |-> 0);
	empty_empty_test: assert property(@(posedge clk_i) x[*0] ##2 y[*0] ##1 z);
	leading_delayed_empty_test: assert property(@(posedge clk_i) ##2 x[*0] ##1 z);
endmodule
