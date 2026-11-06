// miter.v -- REFERENCE SOLUTION (spoilers!): equivalence miter between the
// psp-mini implementation (rtl/psp_mini.v) and the ISA spec (spec_core.v).
//
// The only free input is `instr`: the formal tool may put ANY 32-bit word on
// the instruction port in ANY cycle.  The miter remembers the word the
// implementation actually fetched, and when the implementation commits it
// steps the spec with that same word.  Both machines update their
// architectural state at the same clock edge, so whenever the implementation
// is about to fetch the next instruction the two states must be identical.
//
// Timeline of one instruction (4 cycles in psp-mini):
//
//   cycle    FETCH          DECODE    EXEC     WB(commit)       FETCH ...
//   impl     ir<=instr      ...       ...      state update     ir<=instr
//   miter    CHECK + SYNC                      spec.step        CHECK + SYNC
//            f_insn<=instr                                      f_insn<=instr
//
// Re-anchoring ("sync"): right after the CHECK, the spec's state is overwritten
// with the implementation's state.  In every trace where the CHECK passed the
// two states were already equal, so the sync changes nothing; the first CHECK
// that fails is exactly the same with or without sync.  But it makes the SAT
// problem far easier: the spec now always starts an instruction from the very
// same variables as the implementation, instead of from its own copy whose
// equality the solver would have to re-derive through every earlier
// instruction (try `make bmc NOSYNC=1` to feel the difference).
//
// Defines:  ALU_ONLY  restrict the instruction stream to LUI/ADDI/ADD/XOR/SRLI
//                     (Parts 1-2, before the spec knows the other instructions)
//           NOSYNC    disable re-anchoring (pure lock-step miter)
`include "psp_defs.vh"

module miter (
    input wire        clk,
    input wire [31:0] instr          // free: chosen by the solver every cycle
);
    // --------------------------------------------------------------- reset --
    // `booted` starts at 0 (explicit init value; yosys `sat -set-init-zero`
    // zeroes every other flip-flop as well), so both cores are in reset during
    // step 1 and run from step 2 on.
    reg  booted = 1'b0;
    always @(posedge clk) booted <= 1'b1;
    wire rst = !booted;

    // ------------------------------------------------ the implementation --
    wire        i_imem_en, i_commit;
    wire [31:0] i_imem_addr, i_commit_pc, i_commit_insn;
    wire [31:0] i_pc, i_mtvec, i_mepc;
    wire [3:0]  i_mcause;
    wire        i_priv, i_mpp;
    wire [32*`NREGS-1:0]      i_regs;
    wire [32*`DMEM_WORDS-1:0] i_dmem;

    psp_mini impl (
        .clk(clk), .rst(rst),
        .imem_en(i_imem_en), .imem_addr(i_imem_addr), .imem_rdata(instr),
        .commit(i_commit), .commit_pc(i_commit_pc), .commit_insn(i_commit_insn),
        .dbg_pc(i_pc), .dbg_priv(i_priv), .dbg_regs(i_regs),
        .dbg_mtvec(i_mtvec), .dbg_mepc(i_mepc), .dbg_mcause(i_mcause),
        .dbg_mpp(i_mpp), .dbg_dmem(i_dmem)
    );

    // the instruction word the implementation fetched, and from where
    reg [31:0] f_insn, f_pc;
    always @(posedge clk) begin
        if (rst) begin
            f_insn <= 32'd0;
            f_pc   <= 32'd0;
        end else if (i_imem_en) begin
            f_insn <= instr;
            f_pc   <= i_imem_addr;
        end
    end

    // ----------------------------------------------------------- the spec --
`ifdef NOSYNC
    wire s_sync = 1'b0;
`else
    wire s_sync = i_imem_en;
`endif
    wire        s_step = i_commit;
    wire [31:0] s_pc, s_mtvec, s_mepc;
    wire [3:0]  s_mcause;
    wire        s_priv, s_mpp;
    wire [32*`NREGS-1:0]      s_regs;
    wire [32*`DMEM_WORDS-1:0] s_dmem;

    spec_core spec (
        .clk(clk), .rst(rst),
        .step(s_step), .insn(f_insn), .spec_pc(s_pc),
        .sync(s_sync), .sync_pc(i_pc), .sync_priv(i_priv), .sync_regs(i_regs),
        .sync_mtvec(i_mtvec), .sync_mepc(i_mepc), .sync_mcause(i_mcause),
        .sync_mpp(i_mpp), .sync_dmem(i_dmem),
        .dbg_priv(s_priv), .dbg_regs(s_regs),
        .dbg_mtvec(s_mtvec), .dbg_mepc(s_mepc), .dbg_mcause(s_mcause),
        .dbg_mpp(s_mpp), .dbg_dmem(s_dmem)
    );

    // ------------------------------------------------------ properties --
    // (1) at commit: the implementation retires the word it fetched, from the
    //     address the spec executes it at
    always @* begin
        if (!rst && i_commit) begin
            assert (i_commit_insn == f_insn);
            assert (i_commit_pc   == s_pc);
            assert (f_pc          == s_pc);
        end
    end

    // (2) at every instruction boundary (impl about to fetch; this includes the
    //     first cycle after reset): identical architectural state
    always @* begin
        if (!rst && i_imem_en) begin
            assert (i_pc     == s_pc);
            assert (i_priv   == s_priv);
            assert (i_regs   == s_regs);
            assert (i_mtvec  == s_mtvec);
            assert (i_mepc   == s_mepc);
            assert (i_mcause == s_mcause);
            assert (i_mpp    == s_mpp);
            assert (i_dmem   == s_dmem);
        end
    end

    // (3) bounded liveness: the implementation keeps committing
    reg [3:0] idle;           // cycles since the last commit (saturating)
    always @(posedge clk)
        idle <= (rst || i_commit) ? 4'd0 : (idle == 4'd15 ? idle : idle + 4'd1);
    always @* begin
        if (!rst) assert (idle < 4'd8);
    end

    // ----------------------------------------------------- assumptions --
`ifdef ALU_ONLY
    function is_alu;
        input [31:0] w;
        begin
            is_alu = (w[6:0] == `OP_LUI) ||
                     (w[6:0] == `OP_IMM && w[14:12] == 3'b000) ||                      // ADDI
                     (w[6:0] == `OP_IMM && w[14:12] == 3'b101 && w[31:25] == 7'd0) ||  // SRLI
                     (w[6:0] == `OP_REG && w[31:25] == 7'd0 &&
                      (w[14:12] == 3'b000 || w[14:12] == 3'b100));                    // ADD, XOR
        end
    endfunction
    always @* assume (is_alu(instr));
`endif
endmodule
