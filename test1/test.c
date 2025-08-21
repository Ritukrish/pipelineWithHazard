#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define NUM_REGS 16
#define LINE_LEN 64
#define MAX_INST 256

typedef enum { OP_NOOP, OP_MOV, OP_ADD, OP_SUB, OP_MUL } OpCode;

typedef struct {
    OpCode op;
    int rd, rs1, rs2;   // -1 if not used
    int imm;            // used for MOV
    int valid;          // 1 if this instruction slot contains a real inst
    char text[LINE_LEN];
} Instruction;

typedef struct {
    Instruction inst;
    int alu_result;     // result computed in EX stage (or MOV imm)
    int val_rs1;        // resolved operand 1 (after forwarding)
    int val_rs2;        // resolved operand 2 (after forwarding)
} StageLatch;

// Global register file and program memory
int R[NUM_REGS];
Instruction program[MAX_INST];
int inst_count = 0;

// Pipeline latches (current cycle)
StageLatch IF_latch, ID_latch, EX_latch, MEM_latch, WB_latch;
int PC = 0;

// ---- Helpers ----
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

void init_pipeline() {
    IF_latch.inst = make_nop(); IF_latch.alu_result = 0; IF_latch.val_rs1 = IF_latch.val_rs2 = 0;
    ID_latch.inst = make_nop(); ID_latch.alu_result = 0; ID_latch.val_rs1 = ID_latch.val_rs2 = 0;
    EX_latch.inst = make_nop(); EX_latch.alu_result = 0; EX_latch.val_rs1 = EX_latch.val_rs2 = 0;
    MEM_latch.inst = make_nop(); MEM_latch.alu_result = 0; MEM_latch.val_rs1 = MEM_latch.val_rs2 = 0;
    WB_latch.inst = make_nop(); WB_latch.alu_result = 0; WB_latch.val_rs1 = WB_latch.val_rs2 = 0;
}

// Parse a single line into Instruction
Instruction parse_line(char *line) {
    Instruction ins;
    ins.op = OP_NOOP;
    ins.rd = ins.rs1 = ins.rs2 = -1;
    ins.imm = 0;
    ins.valid = 0;
    line[strcspn(line, "\n")] = 0;
    while(*line == ' ' || *line == '\t') line++;
    if (strlen(line) == 0) {
        return ins;
    }
    strcpy(ins.text, line);
    char opstr[8];
    if (sscanf(line, "%7s", opstr) != 1) return ins;
    if (strcmp(opstr, "mov") == 0 || strcmp(opstr, "MOV") == 0) {
        ins.op = OP_MOV;
        // mov R1, 5
        if (sscanf(line, "%*s R%d , %d", &ins.rd, &ins.imm) < 2) {
            // try without spaces
            sscanf(line, "%*s R%d, %d", &ins.rd, &ins.imm);
        }
        ins.valid = 1;
    } else if (strcmp(opstr, "add") == 0 || strcmp(opstr, "ADD") == 0) {
        ins.op = OP_ADD;
        sscanf(line, "%*s R%d , R%d , R%d", &ins.rd, &ins.rs1, &ins.rs2);
        if (ins.rs1 == -1) sscanf(line, "%*s R%d, R%d, R%d", &ins.rd, &ins.rs1, &ins.rs2);
        ins.valid = 1;
    } else if (strcmp(opstr, "sub") == 0 || strcmp(opstr, "SUB") == 0) {
        ins.op = OP_SUB;
        sscanf(line, "%*s R%d , R%d , R%d", &ins.rd, &ins.rs1, &ins.rs2);
        if (ins.rs1 == -1) sscanf(line, "%*s R%d, R%d, R%d", &ins.rd, &ins.rs1, &ins.rs2);
        ins.valid = 1;
    } else if (strcmp(opstr, "mul") == 0 || strcmp(opstr, "MUL") == 0) {
        ins.op = OP_MUL;
        sscanf(line, "%*s R%d , R%d , R%d", &ins.rd, &ins.rs1, &ins.rs2);
        if (ins.rs1 == -1) sscanf(line, "%*s R%d, R%d, R%d", &ins.rd, &ins.rs1, &ins.rs2);
        ins.valid = 1;
    } else {
        // Unknown text: treat as NOP
        ins = make_nop();
    }
    return ins;
}

int program_load(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;
    char line[LINE_LEN];
    inst_count = 0;
    while (fgets(line, sizeof(line), f)) {
        Instruction ins = parse_line(line);
        if (ins.valid) {
            program[inst_count++] = ins;
            if (inst_count >= MAX_INST) break;
        }
    }
    fclose(f);
    return 0;
}

// Check if ID stage instruction has a RAW hazard with EX stage destination (EX-use hazard)
// Stall condition: ID uses a source that EX will write (EX valid and EX.rd != -1).
bool detect_ex_hazard(const Instruction *id_inst, const Instruction *ex_inst) {
    if (!id_inst->valid) return false;
    if (!ex_inst->valid) return false;
    if (ex_inst->op == OP_NOOP) return false;
    if (ex_inst->rd == -1) return false;
    // Check if id reads the register produced by EX
    if (id_inst->rs1 != -1 && id_inst->rs1 == ex_inst->rd) return true;
    if (id_inst->rs2 != -1 && id_inst->rs2 == ex_inst->rd) return true;
    return false;
}

// Resolve a source operand value for EX stage using forwarding (priority MEM -> WB -> Reg file)
int resolve_operand(int reg, const StageLatch *mem, const StageLatch *wb) {
    if (reg == -1) return 0;
    // Forward from MEM if MEM writes to this reg
    if (mem->inst.valid && mem->inst.rd == reg && mem->inst.op != OP_NOOP) {
        return mem->alu_result;
    }
    // Forward from WB if WB writes to this reg
    if (wb->inst.valid && wb->inst.rd == reg && wb->inst.op != OP_NOOP) {
        return wb->alu_result;
    }
    // Otherwise read register file
    return R[reg];
}

// Execute ALU for EX latch (fill alu_result and val_rs1/val_rs2)
void execute_stage_compute(StageLatch *ex, const StageLatch *mem, const StageLatch *wb) {
    if (!ex->inst.valid || ex->inst.op == OP_NOOP) {
        ex->alu_result = 0;
        ex->val_rs1 = ex->val_rs2 = 0;
        return;
    }
    // Resolve operands with forwarding from MEM and WB
    ex->val_rs1 = (ex->inst.rs1 != -1) ? resolve_operand(ex->inst.rs1, mem, wb) : 0;
    ex->val_rs2 = (ex->inst.rs2 != -1) ? resolve_operand(ex->inst.rs2, mem, wb) : 0;

    switch (ex->inst.op) {
        case OP_MOV: ex->alu_result = ex->inst.imm; break;
        case OP_ADD: ex->alu_result = ex->val_rs1 + ex->val_rs2; break;
        case OP_SUB: ex->alu_result = ex->val_rs1 - ex->val_rs2; break;
        case OP_MUL: ex->alu_result = ex->val_rs1 * ex->val_rs2; break;
        default: ex->alu_result = 0; break;
    }
}

// Print one stage nicely
void print_stage(const char *name, const StageLatch *s) {
    if (!s->inst.valid || s->inst.op == OP_NOOP) {
        printf("%-6s: %-20s ", name, "NOP");
        return;
    }
    if (s->inst.op == OP_MOV) {
        printf("%-6s: %s (R%d<-%d)   ", name, s->inst.text, s->inst.rd, s->inst.imm);
    } else {
        printf("%-6s: %s (R%d<-R%d,R%d) ", name, s->inst.text, s->inst.rd, s->inst.rs1, s->inst.rs2);
    }
}

// Print pipeline state and registers each cycle
void print_cycle(int cycle) {
    printf("\n================ Cycle %d ================\n", cycle);
    print_stage("IF", &IF_latch); printf("\n");
    print_stage("ID", &ID_latch); printf("\n");
    // For EX, print operand values used (forwarded or reg)
    if (!EX_latch.inst.valid || EX_latch.inst.op == OP_NOOP) {
        printf("EX    : NOP\n");
    } else if (EX_latch.inst.op == OP_MOV) {
        printf("EX    : %s -> result=%d\n", EX_latch.inst.text, EX_latch.alu_result);
    } else {
        printf("EX    : %s -> val_rs1=%d, val_rs2=%d, result=%d\n",
               EX_latch.inst.text, EX_latch.val_rs1, EX_latch.val_rs2, EX_latch.alu_result);
    }
    print_stage("MEM", &MEM_latch); printf("\n");
    print_stage("WB", &WB_latch); printf("\n");

    // Print register file snapshot
    printf("\nRegisters: ");
    for (int i = 0; i < NUM_REGS; ++i) {
        printf("R%-2d=%-5d ", i, R[i]);
        if ((i+1) % 8 == 0) printf("\n          ");
    }
    printf("\n");
}

// Check if pipeline is empty (and no more program to fetch)
bool pipeline_empty() {
    if (IF_latch.inst.valid) return false;
    if (ID_latch.inst.valid) return false;
    if (EX_latch.inst.valid) return false;
    if (MEM_latch.inst.valid) return false;
    if (WB_latch.inst.valid) return false;
    return true;
}

int main() {
    // initialize registers
    for (int i = 0; i < NUM_REGS; ++i) R[i] = 0;

    // load program from inst.txt
    if (program_load("inst.txt") != 0) {
        fprintf(stderr, "Could not open inst.txt. Create it with instructions like:\n");
        fprintf(stderr, "mov R1, 5\nmov R2, 10\nadd R3, R1, R2\nmul R4, R3, R2\n");
        return 1;
    }

    // init pipeline latches
    init_pipeline();

    int cycle = 1;

    // Run until program fetched and pipeline drained
    while (PC < inst_count || !pipeline_empty()) {
        // ---- WRITE BACK stage: commit result in WB_latch to register file ----
        if (WB_latch.inst.valid && WB_latch.inst.op != OP_NOOP) {
            if (WB_latch.inst.op == OP_MOV) {
                if (WB_latch.inst.rd >= 0) R[WB_latch.inst.rd] = WB_latch.alu_result;
            } else {
                if (WB_latch.inst.rd >= 0) R[WB_latch.inst.rd] = WB_latch.alu_result;
            }
        }

        // ---- Compute EX stage ALU using current MEM and WB for forwarding ----
        // Note: We compute EX result using current MEM_latch and WB_latch values
        execute_stage_compute(&EX_latch, &MEM_latch, &WB_latch);

        // ---- Hazard detection: if ID depends on EX destination, stall (insert NOP) ----
        bool stall = detect_ex_hazard(&ID_latch.inst, &EX_latch.inst);

        // ---- Prepare next-cycle latches (do not overwrite current ones until done) ----
        StageLatch nextWB = MEM_latch;   // MEM -> WB
        StageLatch nextMEM = EX_latch;   // EX -> MEM (value already computed above)
        StageLatch nextEX;
        StageLatch nextID;
        StageLatch nextIF;

        if (stall) {
            // Insert bubble in EX (NOP), keep ID same (stall), and hold IF (no new fetch)
            nextEX.inst = make_nop();
            nextEX.alu_result = 0; nextEX.val_rs1 = nextEX.val_rs2 = 0;

            nextID = ID_latch;  // ID remains same (instruction stalled)
            nextIF = IF_latch;  // hold IF (refetch later)
        } else {
            // Normal flow: ID -> EX, IF -> ID, and fetch new IF if program remains
            nextEX = ID_latch;
            nextEX.alu_result = 0; nextEX.val_rs1 = nextEX.val_rs2 = 0;

            nextID = IF_latch;

            // fetch new IF
            if (PC < inst_count) {
                nextIF.inst = program[PC++];
                nextIF.alu_result = 0; nextIF.val_rs1 = nextIF.val_rs2 = 0;
            } else {
                nextIF.inst = make_nop();
                nextIF.alu_result = 0; nextIF.val_rs1 = nextIF.val_rs2 = 0;
            }
        }

        // ---- Commit the latch updates ----
        WB_latch = nextWB;
        MEM_latch = nextMEM;
        EX_latch = nextEX;
        ID_latch = nextID;
        IF_latch = nextIF;

        // ---- For the EX latch that moved into MEM we already have alu_result set.
        // The newly moved EX (from ID) will have alu_result computed next cycle. ----

        // ---- Print cycle state (showing values used in EX after previous compute) ----
        print_cycle(cycle);

        cycle++;
    }

    // Final summary
    printf("\n=============== FINAL REGISTER STATE ===============\n");
    for (int i = 0; i < NUM_REGS; ++i) {
        printf("R%-2d=%-5d ", i, R[i]);
        if ((i+1) % 8 == 0) printf("\n");
    }
    printf("\nTotal cycles: %d\n", cycle-1);

    

    return 0;
}
