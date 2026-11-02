module default_disable(input clk_i, input logic rst, input logic rst2, input logic a, input logic b);
	default disable iff rst;

	reg gate = 0;
	reg gate2 = 0;
	reg rst_d1;
	reg rst_d2;
	reg a_d1;
	reg a_d2;

	property named_disable;
		disable iff (rst2) a;
	endproperty

	always @(posedge clk_i) begin
		simple_ref: assert(rst || a);
		nonoverlap_ref: assert(!gate || rst || rst_d1 || !a_d1 || b);
		explicit_ref: assert(rst2 || a);
		named_ref: assert(rst2 || a);
		long_ref: assert(!gate2 || rst_d2 || (a_d2 && (rst_d1 || rst || b)));
		procedural_ref: assert(rst || a);
		procedural_test: assert property(a);

		gate <= 1;
		gate2 <= gate;
		rst_d1 <= rst;
		rst_d2 <= rst_d1;
		a_d1 <= a;
		a_d2 <= a_d1;
	end

	simple_test: assert property(@(posedge clk_i) a);
	nonoverlap_test: assert property(@(posedge clk_i) a |=> b);
	long_test: assert property(@(posedge clk_i) a ##2 b);
	explicit_test: assert property(@(posedge clk_i) disable iff (rst2) a);
	named_test: assert property(@(posedge clk_i) named_disable);
endmodule
