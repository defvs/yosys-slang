module onehot_test #(parameter int WIDTH = 8) (input logic [WIDTH-1:0] val);
    function automatic bit reference_onehot(logic [WIDTH-1:0] v);
        int reference_count = 0;
        for (int i = 0; i < WIDTH; i++)
            reference_count += v[i];
        if (reference_count == 1) return 1;
        else return 0;
    endfunction

    always_comb if (^val !== 'x) begin
        assert($onehot(val) == reference_onehot(val));
    end
endmodule

module onehot_1bit(input logic val);
    onehot_test #(.WIDTH(1)) t(.val(val));
endmodule

module onehot_8bit(input logic [7:0] val);
    onehot_test #(.WIDTH(8)) t(.val(val));
endmodule

module onehot_32bit(input logic [31:0] val);
    onehot_test #(.WIDTH(32)) t(.val(val));
endmodule

module onehot_constants;
    always_comb begin
        assert($onehot(1'b1));
        assert(!$onehot(1'b0));
        assert(!$onehot(8'b00000000));
        assert($onehot(8'b00000001));
        assert($onehot(8'b10000000));
        assert(!$onehot(8'b11111111));
        assert(!$onehot(5'b10101));
    end
endmodule
