module empty_property_default_disable(input clk_i, input logic a, input logic r);
	default disable iff r;

	always @(posedge clk_i) begin
		toplevel_ref: assert(r);
		overlap_ref: assert(r || !a);
	end

	toplevel_test: assert property(@(posedge clk_i) 1'b0[*0:1]);
	overlap_test: assert property(@(posedge clk_i) a |-> 1'b0[*0:1]);
endmodule
