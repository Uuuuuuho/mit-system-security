// miter.v -- equivalence miter: psp-mini implementation vs. ISA spec
//            (STUDENT FILE)
//
// The only free input is `instr`: the formal tool may put ANY 32-bit word on
// the instruction port in ANY cycle.  Your job:
//
//   (a) remember the word the implementation fetched (and from which address),
//   (b) step the spec with that word when the implementation commits it,
//   (c) assert that the two machines agree:
//         - at commit: the committed instruction word and its pc
//         - at every instruction boundary (impl about to fetch, including the
//           first cycle after reset): pc, registers, data memory (Part 1),
//           privilege and CSRs (Part 3)
//         - bounded liveness: the implementation commits at least every 8 cycles
//           (Part 3)
//
// Timeline of one instruction (4 cycles in psp-mini):
//
//   cycle    FETCH          DECODE    EXEC     WB(commit)       FETCH ...
//   impl     ir<=instr      ...       ...      state update     ir<=instr
//   miter    CHECK + SYNC                      spec.step        CHECK + SYNC
//            f_insn<=instr                                      f_insn<=instr
//
// Re-anchoring ("sync", provided below): right after the CHECK, the spec's state
// is overwritten with the implementation's state.  Whenever the CHECK passed the
// two states were already equal, so the first failing CHECK is the same with or
// without sync -- but the SAT problem gets much easier.  README explains why;
// `make bmc NOSYNC=1` turns it off.
//
// Use only immediate assertions inside always @* blocks:
//     always @* if (condition) assert (expression);
// (yosys `read_verilog -formal` turns them into $assert cells).
//
// Defines:  ALU_ONLY  restrict the instruction stream to LUI/ADDI/ADD/XOR/SRLI
//           NOSYNC    disable re-anchoring
`include "psp_defs.vh"

module miter (
    input wire        clk,
    input wire [31:0] instr          // free: chosen by the solver every cycle
);
    // --------------------------------------------------------------- reset --
    // `booted` starts at 0, so both cores are in reset during step 1.
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

    // TODO (Part 1, a): latch the instruction word the implementation fetched
    // (it samples `instr` while i_imem_en is high) and the address it came from.
    reg [31:0] f_insn, f_pc;
    always @(posedge clk) begin
        f_insn <= 32'd0;
        f_pc   <= 32'd0;
    end

    // ----------------------------------------------------------- the spec --
`ifdef NOSYNC
    wire s_sync = 1'b0;
`else
    wire s_sync = i_imem_en;
`endif
    // TODO (Part 1, b): when must the spec execute f_insn?
    wire        s_step = 1'b0;

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
    // TODO (Part 1, c): at commit, the implementation must retire the word it
    // fetched, at the address the spec executes it at.

    // at every instruction boundary: identical architectural state
    always @* begin
        if (!rst && i_imem_en) begin
            assert (i_pc == s_pc);     // example
            // TODO (Part 1, c): registers and data memory
            // TODO (Part 3):    privilege, mtvec, mepc, mcause, mpp
        end
    end

    // TODO (Part 3): bounded liveness -- count the cycles since the last commit
    // and assert that the count stays below 8.

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
