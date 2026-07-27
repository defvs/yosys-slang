module best_effort_sva(input logic clk, a, b, input logic [7:0] data);
	Supported_A: assert property (@(posedge clk) a |=> b);
	Unsupported_A: assert property (@(posedge clk) a throughout b[->1]);
	Knownness_A: assert property (@(posedge clk) !$isunknown(data));
	Bits_A: assert property (@(posedge clk) $bits(data) == 8);
endmodule
