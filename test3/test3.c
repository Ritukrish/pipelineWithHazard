#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define NUM_REGS 16
#define LINE_LEN 64
#define MAX_INST 256

// ---------- ISA ----------
typedef enum { OP_NOOP, OP_MOV, OP_ADD, OP_SUB, OP_MUL } OpCode;

typedef struct {
    OpCode op;
    int rd, rs1, rs2;   // -1 if not used
    int imm;            // used for MOV
    int valid;          // 1 if this instruction slot contains a real inst
    char text[LINE_LEN];
} Instruction;

// For tracing where an operand came from
typedef enum { SRC_NONE, SRC_REG, SRC_MEM, SRC_WB } FwdSrc;

typedef struct {
    Instruction inst;
    int alu_result;     // result computed in EX stage (or MOV imm)
    int val_rs1;        // resolved operand 1 (after forwarding)
    int val_rs2;        // resolved operand 2 (after forwarding)
    FwdSrc src_rs1;     // SRC_REG/SRC_MEM/SRC_WB/SRC_NONE
    FwdSrc src_rs2;     // SRC_REG/SRC_MEM/SRC_WB/SRC_NONE
} StageLatch;

// ---------- CPU container (no globals) ----------
typedef struct {
    int R[NUM_REGS];               // Register file
    Instruction program[MAX_INST]; // Instruction memory
    int inst_count;                // Number of instructions loaded
    int PC;                        // Program Counter

    // Pipeline latches
    StageLatch IF_ID, ID_EX, EX_MEM, MEM_WB;
} CPU;

// ---------- Helpers ----------
const char* opcode_name(OpCode op) {
    switch(op) {
        case OP_MOV: return "MOV";
        case OP_ADD: return "ADD";
        case OP_SUB: return "SUB";
        case OP_MUL: return "MUL";
        case OP_NOOP: return "NOP";
        default: return "UNK";
    }
}

Instruction make_nop() {
    Instruction i;
    i.op = OP_NOOP;
    i.rd = i.rs1 = i.rs2 = -1;
    i.imm = 0;
    i.valid = 0;
    strcpy(i.text, "NOP");
    return i;
}
Instruction create_invalid_instruction(const char *reason) {
    Instruction ins = make_nop();
    ins.valid = 0;
    snprintf(ins.text, LINE_LEN, "ERROR: %s", reason);
    return ins;
}

StageLatch make_nop_latch() {
    StageLatch s;
    s.inst = make_nop();
    s.alu_result = 0;
    s.val_rs1 = s.val_rs2 = 0;
    s.src_rs1 = s.src_rs2 = SRC_NONE;
    return s;
}

void init_pipeline(CPU* cpu) {
    cpu->IF_ID = make_nop_latch();
    cpu->ID_EX = make_nop_latch();
    cpu->EX_MEM = make_nop_latch();
    cpu->MEM_WB = make_nop_latch();
}

Instruction parse_line(char *line) {
    Instruction ins = make_nop();
    char temp_line[LINE_LEN];
    strcpy(temp_line, line);

    char *opcode_str = strtok(temp_line, " ,\t\n");
    if (!opcode_str) return create_invalid_instruction("Missing opcode");

    if (strcasecmp(opcode_str, "mov") == 0) {
        ins.op = OP_MOV;
        char *rd_str = strtok(NULL, " ,\t\n");
        char *imm_str = strtok(NULL, " ,\t\n");

        if (!rd_str || sscanf(rd_str, "R%d", &ins.rd) != 1 || ins.rd < 0 || ins.rd >= NUM_REGS)
            return create_invalid_instruction("Invalid destination register in MOV");
        if (!imm_str || sscanf(imm_str, "%d", &ins.imm) != 1)
            return create_invalid_instruction("Invalid immediate in MOV");

        ins.valid = 1;
    }
    else if (strcasecmp(opcode_str, "add") == 0 ||
             strcasecmp(opcode_str, "sub") == 0 ||
             strcasecmp(opcode_str, "mul") == 0) {

        if (strcasecmp(opcode_str, "add") == 0) ins.op = OP_ADD;
        else if (strcasecmp(opcode_str, "sub") == 0) ins.op = OP_SUB;
        else ins.op = OP_MUL;

        char *rd_str  = strtok(NULL, " ,\t\n");
        char *rs1_str = strtok(NULL, " ,\t\n");
        char *rs2_str = strtok(NULL, " ,\t\n");

        if (!rd_str  || sscanf(rd_str, "R%d", &ins.rd)  != 1 || ins.rd  < 0 || ins.rd  >= NUM_REGS)
            return create_invalid_instruction("Invalid destination register");
        if (!rs1_str || sscanf(rs1_str,"R%d",&ins.rs1)!= 1 || ins.rs1 < 0 || ins.rs1 >= NUM_REGS)
            return create_invalid_instruction("Invalid source register 1");
        if (!rs2_str || sscanf(rs2_str,"R%d",&ins.rs2)!= 1 || ins.rs2 < 0 || ins.rs2 >= NUM_REGS)
            return create_invalid_instruction("Invalid source register 2");

        ins.valid = 1;
    }
    else {
        return create_invalid_instruction("Unknown opcode");
    }

    strcpy(ins.text, line);
    return ins;
}


int program_load(CPU* cpu, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;
    char line[LINE_LEN];
    cpu->inst_count = 0;
    while (fgets(line, sizeof(line), f) && cpu->inst_count < MAX_INST) {
    Instruction ins = parse_line(line);
    if (ins.valid) {
        cpu->program[cpu->inst_count++] = ins;
    } else {
        fprintf(stderr, "Parse error at line %d: %s\n", cpu->inst_count+1, ins.text);
    }
}

    fclose(f);
    return 0;
}

bool pipeline_is_empty(const CPU* cpu) {
    return !cpu->IF_ID.inst.valid && !cpu->ID_EX.inst.valid &&
           !cpu->EX_MEM.inst.valid && !cpu->MEM_WB.inst.valid;
}

// ---------- IF ----------
void fetch_stage(CPU* cpu, Instruction* fetched_inst) {
    assert(cpu->PC >= 0 && cpu->PC <= cpu->inst_count);  // ✅ PC must be in range

    if (cpu->PC < cpu->inst_count) {
        *fetched_inst = cpu->program[cpu->PC];
    } else {
        *fetched_inst = make_nop();
    }
}


// ---------- Forwarding helper ----------
typedef struct {
    int value;
    FwdSrc src;
} Resolved;

Resolved resolve_operand(const CPU* cpu, int reg) {
    Resolved r; r.value = 0; r.src = SRC_NONE;
    if (reg == -1) return r;

    // MEM (EX/MEM) has higher priority than WB (MEM/WB)
    if (cpu->EX_MEM.inst.valid && cpu->EX_MEM.inst.rd == reg && cpu->EX_MEM.inst.rd != -1) {
        r.value = cpu->EX_MEM.alu_result;
        r.src = SRC_MEM;
        return r;
    }
    if (cpu->MEM_WB.inst.valid && cpu->MEM_WB.inst.rd == reg && cpu->MEM_WB.inst.rd != -1) {
        r.value = cpu->MEM_WB.alu_result;
        r.src = SRC_WB;
        return r;
    }
    r.value = cpu->R[reg];
    r.src = SRC_REG;
    return r;
}

// ---------- ID (pure) ----------
typedef struct {
    StageLatch next;
    bool stall;
    const char* stall_reason;
} DecodeResult;

/*
 * With full ALU-to-ALU forwarding and no loads in this ISA,
 * we do not need stalls. Keep logic here for future loads/branches.
 */
DecodeResult decode_stage(const CPU* cpu, StageLatch if_id, StageLatch id_ex) {
    DecodeResult res;
    res.next = if_id; // pass-through for this simple ISA
    res.stall = false;
    res.stall_reason = NULL;

    // Example of hazard detection skeleton (disabled for this ISA):
    // if (if_id.inst.valid && id_ex.inst.valid && id_ex.inst.rd != -1) {
    //     if (if_id.inst.rs1 == id_ex.inst.rd || if_id.inst.rs2 == id_ex.inst.rd) {
    //         // If previous stage was a LOAD, stall (no loads here).
    //     }
    // }

    return res;
}

// ---------- EX (pure) ----------
typedef struct {
    StageLatch next;     // the latch for EX/MEM
    bool branch_taken;   // true if branch was taken
    int target_pc;       // new PC if branch
    bool valid;          // whether this result is valid
} ExecResult;


ExecResult execute_stage(const CPU* cpu, StageLatch id_ex) {
    ExecResult r;
    r.next = id_ex;
    r.branch_taken = false;
    r.target_pc = -1;
    r.valid = id_ex.inst.valid;

    if (!id_ex.inst.valid || id_ex.inst.op == OP_NOOP) {
        r.next.val_rs1 = r.next.val_rs2 = 0;
        r.next.src_rs1 = r.next.src_rs2 = SRC_NONE;
        r.next.alu_result = 0;
        return r;
    }

    //  Defensive: register index validity
    assert(reg_valid(id_ex.inst.rd));
    assert(reg_valid(id_ex.inst.rs1));
    assert(reg_valid(id_ex.inst.rs2));

    // Forwarding
    Resolved rs1 = resolve_operand(cpu, id_ex.inst.rs1);
    Resolved rs2 = resolve_operand(cpu, id_ex.inst.rs2);


    r.next.val_rs1 = rs1.value;
    r.next.val_rs2 = rs2.value;
    r.next.src_rs1 = rs1.src;
    r.next.src_rs2 = rs2.src;

    assert(id_ex.inst.op >= OP_NOOP && id_ex.inst.op <= OP_MUL);

    switch (id_ex.inst.op) {
        case OP_MOV: r.next.alu_result = id_ex.inst.imm; break;
        case OP_ADD: r.next.alu_result = rs1.value + rs2.value; break;
        case OP_SUB: r.next.alu_result = rs1.value - rs2.value; break;
        case OP_MUL: r.next.alu_result = rs1.value * rs2.value; break;
        default:
            r.next.alu_result = 0;
            break;
    }

    return r;
}


// ---------- MEM (no real memory ops in this ISA) ----------
// ---------- MEM (pure) ----------
typedef struct {
    StageLatch next;
} MemResult;

MemResult memory_stage(StageLatch ex_mem) {
    MemResult r;
    r.next = ex_mem;  // pass-through (no real memory ops in this ISA)
    return r;
}


// ---------- WB ----------
void wb_stage(CPU* cpu) {
    if (cpu->MEM_WB.inst.valid &&
        cpu->MEM_WB.inst.op != OP_NOOP &&
        cpu->MEM_WB.inst.rd != -1) {
        cpu->R[cpu->MEM_WB.inst.rd] = cpu->MEM_WB.alu_result;
    }
}

// ---------- Pipeline advancement ----------
void advance_pipeline(CPU* cpu,
                      ExecResult ex_res,
                      MemResult mem_res,
                      Instruction fetched_inst,
                      DecodeResult dec_res) {
    // Commit WB (already done inside wb_stage)

    // MEM → WB
    cpu->MEM_WB = mem_res.next;

    // EX → MEM
    cpu->EX_MEM = ex_res.next;

    // ID → EX
    if (dec_res.stall)
        cpu->ID_EX = make_nop_latch();
    else
        cpu->ID_EX = cpu->IF_ID;

    // IF → ID
    if (!dec_res.stall) {
        cpu->IF_ID.inst = fetched_inst;
        if (cpu->PC < cpu->inst_count) cpu->PC++;
    }
}


// ---------- Pretty printing ----------
static const char* src_name(FwdSrc s) {
    switch (s) {
        case SRC_NONE: return "-";
        case SRC_REG:  return "RF";
        case SRC_MEM:  return "MEM";
        case SRC_WB:   return "WB";
        default:       return "?";
    }
}

void print_stage_inst(const char *name, const StageLatch *s) {
    if (!s->inst.valid || s->inst.op == OP_NOOP) {
        printf("%-6s: %-20s ", name, "NOP");
        return;
    }
    printf("%-6s: %-20s", name, s->inst.text);
}

void print_cycle_state(const CPU* cpu, int cycle, bool stalled, const char* stall_reason) {
    printf("\n================ Cycle %d ================\n", cycle);

    if (cpu->PC < cpu->inst_count)
        printf("IF    : Fetching '%s'%s\n", cpu->program[cpu->PC].text, stalled ? " (stall→refetch)" : "");
    else
        printf("IF    : Done\n");

    if (stalled) {
        printf("ID    : %-20s (Stalled%s%s)\n",
               cpu->IF_ID.inst.valid ? cpu->IF_ID.inst.text : "NOP",
               stall_reason ? " — " : "",
               stall_reason ? stall_reason : "");
    } else {
        print_stage_inst("ID", &cpu->IF_ID); printf("\n");
    }

    if (!cpu->ID_EX.inst.valid || cpu->ID_EX.inst.op == OP_NOOP) {
        printf("EX    : NOP\n");
    } else if (cpu->ID_EX.inst.op == OP_MOV) {
        printf("EX    : %-20s (imm=%d and result=%d)\n",
               cpu->ID_EX.inst.text, cpu->ID_EX.inst.imm, cpu->ID_EX.alu_result);
    } else {
        printf("EX    : %-20s (R%d=%d[%s], R%d=%d[%s]; result=%d)\n",
               cpu->ID_EX.inst.text,
               cpu->ID_EX.inst.rs1, cpu->ID_EX.val_rs1, src_name(cpu->ID_EX.src_rs1),
               cpu->ID_EX.inst.rs2, cpu->ID_EX.val_rs2, src_name(cpu->ID_EX.src_rs2),
               cpu->ID_EX.alu_result);
    }

    print_stage_inst("MEM", &cpu->EX_MEM); printf("\n");

    if (cpu->MEM_WB.inst.valid && cpu->MEM_WB.inst.rd != -1 && cpu->MEM_WB.inst.op != OP_NOOP) {
        printf("WB    : %-20s (write R%d=%d)\n",
               cpu->MEM_WB.inst.text,
               cpu->MEM_WB.inst.rd,
               cpu->MEM_WB.alu_result);
    } else {
        print_stage_inst("WB", &cpu->MEM_WB); printf("\n");
    }

    // Registers
    printf("\nRegisters: ");
    for (int i = 0; i < NUM_REGS; ++i) {
        printf("R%-2d=%-5d ", i, cpu->R[i]);
        if ((i + 1) % 8 == 0) printf("\n           ");
    }
    printf("\n");
}

// ---------- main ----------
int main() {
    CPU cpu;
    memset(&cpu, 0, sizeof(CPU));
    for (int i = 0; i < NUM_REGS; ++i) cpu.R[i] = 0;
    cpu.PC = 0;

    if (program_load(&cpu, "inst.txt") != 0) {
        fprintf(stderr, "Could not open inst.txt. Please create it.\n");
        return 1;
    }

    init_pipeline(&cpu);
    int cycle = 1;

    // Prime IF_ID with first fetch so the first cycle shows ID properly
    Instruction first;
    fetch_stage(&cpu, &first);
    cpu.IF_ID.inst = first;
    if (cpu.PC < cpu.inst_count) cpu.PC++;

 while (cpu.PC < cpu.inst_count || !pipeline_is_empty(&cpu)) {
    // ---- Phase 1: compute ----
    wb_stage(&cpu);
   MemResult mem_res = memory_stage(cpu.EX_MEM);
ExecResult ex_res = execute_stage(&cpu, cpu.ID_EX);

    DecodeResult dec_res = decode_stage(&cpu, cpu.IF_ID, cpu.ID_EX);
    Instruction fetched_inst;
    fetch_stage(&cpu, &fetched_inst);

    // ---- Phase 2: print ----
    // ---- Phase 2: print ----
// Use the execute result just for printing the EX line
StageLatch saved_id_ex = cpu.ID_EX;
cpu.ID_EX = ex_res.next;

print_cycle_state(&cpu, cycle, dec_res.stall, dec_res.stall_reason);

// Restore the original latched view before we advance
cpu.ID_EX = saved_id_ex;


    // ---- Phase 3: latch update ----
    advance_pipeline(&cpu, ex_res, mem_res, fetched_inst, dec_res);

    cycle++;
}


    // Final summary
    printf("\n=============== FINAL REGISTER STATE ===============\n");
    for (int i = 0; i < NUM_REGS; ++i) {
        printf("R%-2d=%-5d ", i, cpu.R[i]);
        if ((i + 1) % 8 == 0) printf("\n");
    }
    printf("\nTotal cycles: %d\n", cycle - 1);

    return 0;
}


