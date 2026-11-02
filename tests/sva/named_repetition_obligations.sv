// A repetition of a named sequence repeats all of it, `s[*2]` is `s ##1 s`, also where
// the property gets taken apart into obligations as it does under `if` and `case`
module named_repetition_obligations(input clk_i, input logic a, input logic b, input logic c);
	sequence s;
		a ##1 b;
	endsequence

	property twice;
		s[*2];
	endproperty

	reg gate1 = 0;
	reg gate2 = 0;
	reg gate3 = 0;
	reg a_d1;
	reg a_d2;
	reg a_d3;
	reg b_d1;
	reg b_d2;
	reg c_d1;
	reg c_d2;
	reg c_d3;

	always @(posedge clk_i) begin
		if_ref: assert(!gate3 || !c_d3 || (a_d3 && b_d2 && a_d1 && b));
		instance_ref: assert(!gate3 || !c_d3 || (a_d3 && b_d2 && a_d1 && b));
		range_ref: assert(!gate3 || !c_d3 || (a_d3 && b_d2));
		case_ref: assert(!gate3 || !c_d3 || (a_d3 && b_d2 && a_d1 && b));

		gate1 <= 1;
		gate2 <= gate1;
		gate3 <= gate2;
		a_d1 <= a;
		a_d2 <= a_d1;
		a_d3 <= a_d2;
		b_d1 <= b;
		b_d2 <= b_d1;
		c_d1 <= c;
		c_d2 <= c_d1;
		c_d3 <= c_d2;
	end

	if_test: assert property(@(posedge clk_i) if (c) s[*2]);
	instance_test: assert property(@(posedge clk_i) if (c) twice);
	range_test: assert property(@(posedge clk_i) if (c) s[*1:2]);
	case_test: assert property(@(posedge clk_i) case (c) 1'b1: s[*2]; endcase);
endmodule
