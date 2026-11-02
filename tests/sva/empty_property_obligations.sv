// The rules of empty_property.sv where the property gets taken apart into obligations:
// an empty match of a sequence used as a property is no success. Only an antecedent
// of `#=#` has its empty match end on the attempt's tick, by way of the `##1 `true`.
module empty_property_obligations(input clk_i, input logic a, input logic b, input logic c);
	always @(posedge clk_i) begin
		if_ref: assert(!c);
		strong_ref: assert(0);
	end

	eventually_ref: assert property(@(posedge clk_i) s_eventually 1'b0);
	followed_ref: assert property(@(posedge clk_i) s_eventually b);

	if_test: assert property(@(posedge clk_i) if (c) 1'b0[*0:1]);
	strong_test: assert property(@(posedge clk_i) strong(1'b0[*0:1]));
	eventually_test: assert property(@(posedge clk_i) s_eventually 1'b0[*0:1]);
	followed_test: assert property(@(posedge clk_i) 1'b0[*0:1] #=# s_eventually b);
endmodule
