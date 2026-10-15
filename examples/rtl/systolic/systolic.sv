// The tpu example's systolic array behind the standard vcix RTL ports: commands (input push, weight push, pop) run in
// order; their data comes on `in` and leaves on `out`, one beat (an element of every lane) a cycle. A weight push waits
// until every older input has left the array (WEIGHT_WAITS=1). BAD_TAG, not 0, is a fault the wrapper's check sees.
module systolic #(
  parameter int L = 4,
  parameter int Q = 64,
  parameter bit WEIGHT_WAITS = 1,
  parameter int BAD_TAG = 0
) (
  input  logic            clk,
  input  logic            rst,
  input  logic            cmd_valid,
  output logic            cmd_ready,
  input  logic [1:0]      cmd_op,
  input  logic [15:0]     cmd_vl,
  input  logic [7:0]      cmd_tag,
  input  logic            in_valid,
  output logic            in_ready,
  input  logic [L*8-1:0]  in_data,
  output logic            out_valid,
  input  logic            out_ready,
  output logic [L*8-1:0]  out_data,
  output logic [7:0]      out_tag,
  output logic            out_last,
  output logic            busy
);
  localparam int SLOTS = 2 * L - 1;
  localparam logic [1:0] INPUT = 0, WEIGHT = 1, POP = 2;

  logic [1:0]  c_op [2];
  logic [15:0] c_vl [2];
  logic [7:0]  c_tag [2];
  logic [1:0]  c_count;
  logic [15:0] done_beats;

  logic signed [7:0]  weight [L][L];
  logic [15:0]        weights;
  logic [L*8-1:0]     in_q [Q];
  logic [15:0]        in_head, in_tail, in_count;
  logic [L*32-1:0]    out_q [Q];
  logic [15:0]        out_head, out_tail, out_count;
  logic signed [7:0]  skew [L][L];
  logic signed [7:0]  xr [L][L];
  logic signed [31:0] psum [L][L];
  logic signed [31:0] desk [L][L];
  logic [SLOTS-1:0]   valid;
  logic [15:0]        in_line;

  logic front, in_fire, out_fire, cmd_fire, enable, enter, retire, weight_open;
  logic signed [7:0]  xin [L];
  logic signed [31:0] result [L];
  logic [L*8-1:0]     x;

  always_comb begin
    front = c_count != 0;
    weight_open = !WEIGHT_WAITS || (in_count == 0 && in_line == 0);
    in_ready = front && ((c_op[0] == INPUT && int'(in_count) < Q) || (c_op[0] == WEIGHT && weight_open));
    out_valid = front && c_op[0] == POP && out_count != 0;
    for (int j = 0; j < L; j++) out_data[j*8 +: 8] = out_q[int'(out_head)][j*32 +: 8];
    out_tag = c_tag[0] + 8'(BAD_TAG);
    out_last = done_beats + 1 == c_vl[0];
    in_fire = in_valid && in_ready;
    out_fire = out_valid && out_ready;
    retire = (in_fire || out_fire) && done_beats + 1 == c_vl[0];
    cmd_ready = c_count != 2;
    cmd_fire = cmd_valid && cmd_ready;
    enable = out_count != 16'(Q);
    enter = enable && in_count != 0;
    x = in_q[int'(in_head)];
    xin[0] = enter ? signed'(x[7:0]) : 8'sd0;
    for (int k = 1; k < L; k++) xin[k] = skew[k][k-1];
    for (int j = 0; j < L; j++) result[j] = j == L - 1 ? psum[L-1][L-1] : desk[j][L-2-j];
    busy = enable && (in_count != 0 || in_line != 0);
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      c_count <= 0; done_beats <= 0; weights <= 0;
      in_head <= 0; in_tail <= 0; in_count <= 0; out_head <= 0; out_tail <= 0; out_count <= 0;
      valid <= 0; in_line <= 0;
      for (int j = 0; j < L; j++)
        for (int k = 0; k < L; k++) weight[j][k] <= 0;
    end else begin
      begin : commands
        logic [1:0] n;
        n = c_count;
        if (retire) begin
          c_op[0] <= c_op[1]; c_vl[0] <= c_vl[1]; c_tag[0] <= c_tag[1];
          n = n - 1;
          done_beats <= 0;
        end else if (in_fire || out_fire) begin
          done_beats <= done_beats + 1;
        end
        if (cmd_fire) begin
          c_op[int'(n)] <= cmd_op; c_vl[int'(n)] <= cmd_vl; c_tag[int'(n)] <= cmd_tag;
          n = n + 1;
        end
        c_count <= n;
      end
      if (in_fire && c_op[0] == WEIGHT) begin
        for (int j = 0; j < L; j++) begin
          if (int'(weights) < L) weight[j][int'(weights)] <= signed'(in_data[j*8 +: 8]);
          else begin
            for (int k = 0; k < L - 1; k++) weight[j][k] <= weight[j][k+1];
            weight[j][L-1] <= signed'(in_data[j*8 +: 8]);
          end
        end
        if (int'(weights) < L) weights <= weights + 1;
      end
      begin : queues
        logic [15:0] ic, oc;
        ic = in_count; oc = out_count;
        if (in_fire && c_op[0] == INPUT) begin
          in_q[int'(in_tail)] <= in_data;
          in_tail <= 16'((int'(in_tail) + 1) % Q);
          ic = ic + 1;
        end
        if (out_fire) begin
          out_head <= 16'((int'(out_head) + 1) % Q);
          oc = oc - 1;
        end
        if (enable) begin
          if (valid[SLOTS-1]) begin
            for (int j = 0; j < L; j++) out_q[int'(out_tail)][j*32 +: 32] <= result[j];
            out_tail <= 16'((int'(out_tail) + 1) % Q);
            oc = oc + 1;
          end
          if (enter) begin
            in_head <= 16'((int'(in_head) + 1) % Q);
            ic = ic - 1;
          end
          valid <= {valid[SLOTS-2:0], enter};
          in_line <= in_line + {15'd0, enter} - {15'd0, valid[SLOTS-1]};
        end
        in_count <= ic; out_count <= oc;
      end
      if (enable) begin
        for (int k = 1; k < L; k++) begin
          skew[k][0] <= enter ? signed'(x[k*8 +: 8]) : 8'sd0;
          for (int d = 1; d < L; d++) skew[k][d] <= skew[k][d-1];
        end
        for (int k = 0; k < L; k++)
          for (int j = 0; j < L; j++) begin
            xr[k][j] <= j == 0 ? xin[k] : xr[k][j-1];
            psum[k][j] <= (k == 0 ? 32'sd0 : psum[k-1][j]) + 32'(j == 0 ? xin[k] : xr[k][j-1]) * 32'(weight[j][k]);
          end
        for (int j = 0; j < L - 1; j++) begin
          desk[j][0] <= psum[L-1][j];
          for (int d = 1; d < L - 1 - j; d++) desk[j][d] <= desk[j][d-1];
        end
      end
    end
  end
endmodule
