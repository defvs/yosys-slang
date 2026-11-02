module default_disable(input clk_i, input logic rst, input logic rst2, input logic a, input logic b);
	default disable iff rst;

	reg gate = 0;
	reg rst_d1;
	reg a_d1;

	property named_disable;
		disable iff (rst2) a;
	endproperty

	always @(posedge clk_i) begin
		simple_ref: assert(rst || a);
		nonoverlap_ref: assert(!gate || rst || rst_d1 || !a_d1 || b);
		explicit_ref: assert(rst2 || a);
		named_ref: assert(rst2 || a);
		procedural_ref: assert(rst || a);
		procedural_test: assert property(a);

		gate <= 1;
		rst_d1 <= rst;
		a_d1 <= a;
	end

	simple_test: assert property(@(posedge clk_i) a);
	nonoverlap_test: assert property(@(posedge clk_i) a |=> b);
	explicit_test: assert property(@(posedge clk_i) disable iff (rst2) a);
	named_test: assert property(@(posedge clk_i) named_disable);
endmodule
