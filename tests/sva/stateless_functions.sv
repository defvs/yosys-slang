// The functions of an assertion may assign their static variables, as long as they read
// them only after the call assigned them and so keep no state (IEEE Std 1800-2017
// section 16.6)
package stateless_functions_pkg;
	function logic [3:0] reversed(logic [3:0] x);
		logic [3:0] r;
		r = 4'd0;
		for (int i = 0; i < 4; i++)
			r[i] = x[3 - i];
		return r;
	endfunction
endpackage

module stateless_functions(input logic clk_i, input logic [3:0] v, input logic [1:0] s);
	function logic [3:0] larger(logic [3:0] x, logic [3:0] y);
		if (x > y)
			larger = x;
		else
			larger = y;
	endfunction

	function logic [3:0] decode(logic [1:0] sel);
		logic [3:0] onehot;
		case (sel)
			2'd0: onehot = 4'b0001;
			2'd1: onehot = 4'b0010;
			default: onehot = 4'b0100 << (sel - 2'd2);
		endcase
		return onehot;
	endfunction

	function automatic logic [3:0] shifted_down(logic [3:0] x);
		static logic [3:0] scratch;
		scratch = x;
		for (int i = 0; i < 3; i++)
			if (scratch[0] == 1'b0)
				scratch = scratch >> 1;
		return scratch;
	endfunction

	function logic [3:0] both(logic [3:0] x);
		return larger(stateless_functions_pkg::reversed(x), x);
	endfunction

	always @(posedge clk_i) begin
		branches_ref: assert((v > {2'b00, s} ? v : {2'b00, s}) != 4'd2);
		case_ref: assert((4'b0001 << s) != v);
		automatic_static_ref: assert((v == 4'd0 ? 4'd0 : v[0] ? v : v[1] ? v >> 1 :
				v[2] ? v >> 2 : v >> 3) != 4'd1);
		loop_ref: assert({v[0], v[1], v[2], v[3]} != 4'b0011);
		nested_ref: assert(({v[0], v[1], v[2], v[3]} > v ? {v[0], v[1], v[2], v[3]} : v) != 4'd6);
	end

	branches_test: assert property(@(posedge clk_i) larger(v, {2'b00, s}) != 4'd2);
	case_test: assert property(@(posedge clk_i) decode(s) != v);
	automatic_static_test: assert property(@(posedge clk_i) shifted_down(v) != 4'd1);
	loop_test: assert property(@(posedge clk_i) stateless_functions_pkg::reversed(v) != 4'b0011);
	nested_test: assert property(@(posedge clk_i) both(v) != 4'd6);
endmodule
