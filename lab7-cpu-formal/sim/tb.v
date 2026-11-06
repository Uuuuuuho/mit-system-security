// tb.v -- iverilog testbench: runs a program on psp-mini and, in lock-step,
// on the spec core.  This is the *simulation* counterpart of the miter: the
// same "step the spec when the implementation commits, compare at the next
// instruction boundary" idea, but driven by one concrete program instead of by
// a SAT solver.
//
// Plusargs:  +hex=<file>        program image (one 32-bit hex word per line)
//            +maxcycles=<n>     give up after n cycles (default 20000)
//            +nospec            do not compare against the spec
//            +trace             print every committed instruction
// Output lines (parsed by tools/sim.py):
//   COMMIT  <n> pc=<hex> insn=<hex>                  (with +trace)
//   MISMATCH <n> pc=<hex> insn=<hex> <field> impl=<hex> spec=<hex>
//   FINAL pc=.. priv=.. x0=.. x1=.. ... mem0=.. ... cycles=.. commits=.. halted=..
`timescale 1ns/1ps
`include "psp_defs.vh"

module tb;
    reg clk = 1'b0;
    always #5 clk = ~clk;
    reg rst = 1'b1;

    // ------------------------------------------------ instruction ROM --
    reg [31:0] rom [0:255];
    reg [8*256-1:0] hexfile;
    integer k;
    initial begin
        for (k = 0; k < 256; k = k + 1) rom[k] = 32'h00000063;   // halt everywhere
        if (!$value$plusargs("hex=%s", hexfile)) begin
            $display("ERROR: no +hex=<file>");
            $finish;
        end
        $readmemh(hexfile, rom);
    end

    wire        imem_en;
    wire [31:0] imem_addr;
    wire [31:0] instr = rom[imem_addr[9:2]];

    // ------------------------------------------------ implementation --
    wire        commit;
    wire [31:0] commit_pc, commit_insn;
    wire [31:0] i_pc, i_mtvec, i_mepc;
    wire [3:0]  i_mcause;
    wire        i_priv, i_mpp;
    wire [32*`NREGS-1:0]      i_regs;
    wire [32*`DMEM_WORDS-1:0] i_dmem;

    psp_mini impl (
        .clk(clk), .rst(rst),
        .imem_en(imem_en), .imem_addr(imem_addr), .imem_rdata(instr),
        .commit(commit), .commit_pc(commit_pc), .commit_insn(commit_insn),
        .dbg_pc(i_pc), .dbg_priv(i_priv), .dbg_regs(i_regs),
        .dbg_mtvec(i_mtvec), .dbg_mepc(i_mepc), .dbg_mcause(i_mcause),
        .dbg_mpp(i_mpp), .dbg_dmem(i_dmem)
    );

    reg [31:0] f_insn;
    always @(posedge clk) if (imem_en) f_insn <= instr;

    // ------------------------------------------------------- spec --
    wire [31:0] s_pc, s_mtvec, s_mepc;
    wire [3:0]  s_mcause;
    wire        s_priv, s_mpp;
    wire [32*`NREGS-1:0]      s_regs;
    wire [32*`DMEM_WORDS-1:0] s_dmem;

    spec_core spec (
        .clk(clk), .rst(rst),
        .step(commit), .insn(f_insn), .spec_pc(s_pc),
        .sync(1'b0), .sync_pc(32'd0), .sync_priv(1'b0), .sync_regs({32*`NREGS{1'b0}}),
        .sync_mtvec(32'd0), .sync_mepc(32'd0), .sync_mcause(4'd0), .sync_mpp(1'b0),
        .sync_dmem({32*`DMEM_WORDS{1'b0}}),
        .dbg_priv(s_priv), .dbg_regs(s_regs),
        .dbg_mtvec(s_mtvec), .dbg_mepc(s_mepc), .dbg_mcause(s_mcause),
        .dbg_mpp(s_mpp), .dbg_dmem(s_dmem)
    );

    // ---------------------------------------------------- checking --
    integer cycles = 0, commits = 0, maxcycles = 20000;
    reg nospec, trace, halted = 1'b0, mismatch = 1'b0;
    reg [31:0] last_pc = 32'd0, last_insn = 32'd0;
    initial begin
        nospec = $test$plusargs("nospec");
        trace  = $test$plusargs("trace");
        if (!$value$plusargs("maxcycles=%d", maxcycles)) maxcycles = 20000;
        repeat (2) @(posedge clk);
        #1 rst = 1'b0;
    end

    task report_field;
        input [8*8-1:0] name;
        input [127:0] iv, sv;
        begin
            if (iv !== sv) begin
                $display("MISMATCH %0d pc=%08h insn=%08h %0s impl=%0h spec=%0h",
                         commits, last_pc, last_insn, name, iv, sv);
                mismatch = 1'b1;
            end
        end
    endtask

    task dump_final;
        integer r;
        begin
            $write("FINAL pc=%08h priv=%0d", i_pc, i_priv);
            for (r = 0; r < `NREGS; r = r + 1) $write(" x%0d=%08h", r, i_regs[32*r +: 32]);
            $write(" mtvec=%08h mepc=%08h mcause=%0d mpp=%0d", i_mtvec, i_mepc, i_mcause, i_mpp);
            for (r = 0; r < `DMEM_WORDS; r = r + 1) $write(" mem%0d=%08h", r, i_dmem[32*r +: 32]);
            $display(" cycles=%0d commits=%0d halted=%0d", cycles, commits, halted);
        end
    endtask

    always @(posedge clk) begin
        if (!rst) begin
            cycles = cycles + 1;
            if (commit) begin
                commits   = commits + 1;
                last_pc   = commit_pc;
                last_insn = commit_insn;
                if (trace) $display("COMMIT %0d pc=%08h insn=%08h", commits, commit_pc, commit_insn);
                if (commit_insn == `INSN_HALT) halted = 1'b1;
            end
            // instruction boundary: the impl is about to fetch.  (The final
            // `halt` is not compared, so ALU-only programs can be co-simulated
            // before the spec knows BEQ.)
            if (imem_en && !nospec && !halted) begin
                report_field("pc",     {96'd0, i_pc},     {96'd0, s_pc});
                report_field("priv",   {127'd0, i_priv},  {127'd0, s_priv});
                report_field("regs",   i_regs, s_regs);      // NREGS*32 = 128 bits
                report_field("mtvec",  {96'd0, i_mtvec},  {96'd0, s_mtvec});
                report_field("mepc",   {96'd0, i_mepc},   {96'd0, s_mepc});
                report_field("mcause", {124'd0, i_mcause},{124'd0, s_mcause});
                report_field("mpp",    {127'd0, i_mpp},   {127'd0, s_mpp});
                report_field("dmem",   i_dmem, s_dmem);
            end
            if (mismatch || (imem_en && halted) || cycles >= maxcycles) begin
                dump_final;
                $finish;
            end
        end
    end
endmodule
