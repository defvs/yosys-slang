module regular_sequence_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire request = phase == 0;
	wire prefix = request;
	wire progress = phase == 2;

	assert property (@(posedge clk)
		request |-> strong(prefix ##[0:$] progress));
	cover property (@(posedge clk) request);
endmodule

module regular_sequence_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire request = phase == 0;
	wire prefix = request;
	wire progress = 1'b0;

	assert property (@(posedge clk)
		request |-> strong(prefix ##[0:$] progress));
	cover property (@(posedge clk) request);
endmodule

module regular_intersect_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 0;
	assert property (@(posedge clk)
		strong((##[0:$] a) intersect (##[0:$] b)));
	cover property (@(posedge clk) a && b);
endmodule

module regular_intersect_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 1;
	assert property (@(posedge clk)
		strong((##[0:$] a) intersect (##[0:$] b)));
	cover property (@(posedge clk) a);
endmodule

module regular_goto_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	assert property (@(posedge clk) strong(a[->2]));
	cover property (@(posedge clk) a);
endmodule

module regular_goto_fail ((* gclk *) input logic clk);
	wire a = 1'b0;
	assert property (@(posedge clk) strong(a[->2]));
	cover property (@(posedge clk) !a);
endmodule

module regular_within_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase == 2 ? 0 : phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 1;
	wire c = phase == 2;
	assert property (@(posedge clk)
		strong((a ##1 b) within (##[0:$] c)));
	cover property (@(posedge clk) c);
endmodule

module regular_within_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase == 2 ? 0 : phase + 1'b1;

	wire a = phase == 0;
	wire b = 1'b0;
	wire c = phase == 2;
	assert property (@(posedge clk)
		strong((a ##1 b) within (##[0:$] c)));
	cover property (@(posedge clk) c);
endmodule

module regular_and_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 2;
	assert property (@(posedge clk)
		strong((##[0:$] a) and (##[0:$] b)));
	cover property (@(posedge clk) a || b);
endmodule

module regular_and_fail ((* gclk *) input logic clk);
	logic phase = 0;
	always_ff @(posedge clk)
		phase <= !phase;

	wire a = phase == 0;
	wire b = 1'b0;
	assert property (@(posedge clk)
		strong((##[0:$] a) and (##[0:$] b)));
	cover property (@(posedge clk) a);
endmodule

module regular_or_pass ((* gclk *) input logic clk);
	logic phase = 0;
	always_ff @(posedge clk)
		phase <= !phase;

	wire a = phase == 0;
	wire b = 1'b0;
	assert property (@(posedge clk)
		strong((##[0:$] a) or (##[0:$] b)));
	cover property (@(posedge clk) a);
endmodule

module regular_or_fail ((* gclk *) input logic clk);
	wire a = 1'b0;
	wire b = 1'b0;
	assert property (@(posedge clk)
		strong((##[0:$] a) or (##[0:$] b)));
	cover property (@(posedge clk) !a && !b);
endmodule

module regular_throughout_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire guard = 1'b1;
	wire done = phase == 2;
	assert property (@(posedge clk)
		strong(guard throughout (##[0:$] done)));
	cover property (@(posedge clk) done);
endmodule

module regular_throughout_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire guard = phase != 1;
	wire done = phase == 2;
	assert property (@(posedge clk)
		strong(guard throughout (##[0:$] done)));
	cover property (@(posedge clk) done);
endmodule

module regular_nonconsecutive_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	assert property (@(posedge clk) strong(a[=2]));
	cover property (@(posedge clk) a);
endmodule

module regular_nonconsecutive_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		if (phase != 3)
			phase <= phase + 1'b1;

	wire a = phase == 0;
	assert property (@(posedge clk) strong(a[=2]));
	cover property (@(posedge clk) a);
endmodule

module regular_consecutive_pass ((* gclk *) input logic clk);
	wire a = 1'b1;
	assert property (@(posedge clk) strong(a[*2:$]));
	cover property (@(posedge clk) a);
endmodule

module regular_consecutive_fail ((* gclk *) input logic clk);
	logic phase = 0;
	always_ff @(posedge clk)
		phase <= !phase;

	wire a = phase;
	assert property (@(posedge clk) strong(a[*2:$]));
	cover property (@(posedge clk) a);
endmodule

module regular_empty_fusion_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		if (phase != 3)
			phase <= phase + 1'b1;

	wire request = phase == 0;
	wire x = 1'b0;
	wire y = phase == 0;
	wire z = phase == 2;
	assert property (@(posedge clk)
		request |-> strong(x[*0] ##1 y ##[0:$] z));
	cover property (@(posedge clk) request);
endmodule

module regular_empty_fusion_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		if (phase != 3)
			phase <= phase + 1'b1;

	wire request = phase == 0;
	wire x = 1'b0;
	wire y = phase == 1;
	wire z = phase == 2;
	assert property (@(posedge clk)
		request |-> strong(x[*0] ##1 y ##[0:$] z));
	cover property (@(posedge clk) request);
endmodule
