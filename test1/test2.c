#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define NUM_REGS 16
#define LINE_LEN 64
#define MAX_INST 256

// Typedefs and Enums (from original code)
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

// Global state
int R[NUM_REGS];                 // Register file
Instruction program[MAX_INST];   // Instruction memory
int inst_count = 0;              // Number of instructions loaded
int PC = 0;                      // Program Counter

// Pipeline latches, named for the stages they separate
StageLatch IF_ID_latch, ID_EX_latch, EX_MEM_latch, MEM_WB_latch;

// ---- Helper Functions (mostly from original code) ----

// Returns a string name for an opcode
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

// Creates an empty NOP instruction
Instruction make_nop() {
    Instruction i;
    i.op = OP_NOOP;
    i.rd = i.rs1 = i.rs2 = -1;
    i.imm = 0;
    i.valid = 0;
    strcpy(i.text, "NOP");
    return i;
}

// Initializes all pipeline latches to a NOP state
void init_pipeline() {
    IF_ID_latch.inst = make_nop();
    ID_EX_latch.inst = make_nop();
    EX_MEM_latch.inst = make_nop();
    MEM_WB_latch.inst = make_nop();
}

// Parses a single line of assembly into an Instruction struct
Instruction parse_line(char *line) {
    Instruction ins;
    ins.op = OP_NOOP;
    ins.rd = ins.rs1 = ins.rs2 = -1;
    ins.imm = 0;
    ins.valid = 0;
    line[strcspn(line, "\n")] = 0;
    while(*line == ' ' || *line == '\t') line++;
    if (strlen(line) == 0) return ins;

    strcpy(ins.text, line);
    char opstr[8];
    sscanf(line, "%7s", opstr);

    if (strcasecmp(opstr, "mov") == 0) {
        ins.op = OP_MOV;
        sscanf(line, "%*s R%d , %d", &ins.rd, &ins.imm);
        ins.valid = 1;
    } else if (strcasecmp(opstr, "add") == 0) {
        ins.op = OP_ADD;
        sscanf(line, "%*s R%d , R%d , R%d", &ins.rd, &ins.rs1, &ins.rs2);
        ins.valid = 1;
    } else if (strcasecmp(opstr, "sub") == 0) {
        ins.op = OP_SUB;
        sscanf(line, "%*s R%d , R%d , R%d", &ins.rd, &ins.rs1, &ins.rs2);
        ins.valid = 1;
    } else if (strcasecmp(opstr, "mul") == 0) {
        ins.op = OP_MUL;
        sscanf(line, "%*s R%d , R%d , R%d", &ins.rd, &ins.rs1, &ins.rs2);
        ins.valid = 1;
    }
    return ins;
}

// Loads a program from a file into instruction memory
int program_load(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;
    char line[LINE_LEN];
    inst_count = 0;
    while (fgets(line, sizeof(line), f) && inst_count < MAX_INST) {
        Instruction ins = parse_line(line);
        if (ins.valid) {
            program[inst_count++] = ins;
        }
    }
    fclose(f);
    return 0;
}

// Checks if the pipeline is empty (all latches hold NOPs)
bool pipeline_is_empty() {
    return !IF_ID_latch.inst.valid && !ID_EX_latch.inst.valid &&
           !EX_MEM_latch.inst.valid && !MEM_WB_latch.inst.valid;
}

// ---- Pipeline Stage Functions ----

/**
 * @brief Stage 1: Instruction Fetch
 * Fetches the next instruction from memory based on the PC.
 * @param fetched_inst Pointer to store the fetched instruction.
 */
void fetch_stage(Instruction* fetched_inst) {
    if (PC < inst_count) {
        *fetched_inst = program[PC];
    } else {
        *fetched_inst = make_nop();
    }
}

/**
 * @brief Stage 2: Instruction Decode & Hazard Detection
 * Checks for a RAW (Read-After-Write) hazard between the instruction
 * in the decode stage (in IF_ID latch) and the one in execute (ID_EX latch).
 * @return true if a stall is needed, false otherwise.
 */
bool decode_stage() {
    Instruction* id_inst = &IF_ID_latch.inst;
    Instruction* ex_inst = &ID_EX_latch.inst;

    if (!id_inst->valid || !ex_inst->valid || ex_inst->op == OP_NOOP || ex_inst->rd == -1) {
        return false;
    }
    // Check if ID stage reads a register that EX stage writes to.
    if (id_inst->rs1 != -1 && id_inst->rs1 == ex_inst->rd) return true;
    if (id_inst->rs2 != -1 && id_inst->rs2 == ex_inst->rd) return true;

    return false;
}

/**
 * @brief Stage 3: Execute
 * Performs the ALU operation for the instruction in the ID_EX latch.
 * This version includes forwarding logic to reduce stalls (though stalls are still needed).
 */
void execute_stage() {
    StageLatch* ex_latch = &ID_EX_latch;
    if (!ex_latch->inst.valid || ex_latch->inst.op == OP_NOOP) return;

    // Resolve operands with forwarding from MEM and WB stages
    // Forwarding Priority: MEM -> WB -> Register File
    int val1 = R[ex_latch->inst.rs1];
    if (ex_latch->inst.rs1 != -1) {
        if (EX_MEM_latch.inst.valid && EX_MEM_latch.inst.rd == ex_latch->inst.rs1) {
             val1 = EX_MEM_latch.alu_result; // Forward from MEM
        } else if (MEM_WB_latch.inst.valid && MEM_WB_latch.inst.rd == ex_latch->inst.rs1) {
             val1 = MEM_WB_latch.alu_result; // Forward from WB
        }
    }

    int val2 = R[ex_latch->inst.rs2];
    if (ex_latch->inst.rs2 != -1) {
        if (EX_MEM_latch.inst.valid && EX_MEM_latch.inst.rd == ex_latch->inst.rs2) {
             val2 = EX_MEM_latch.alu_result; // Forward from MEM
        } else if (MEM_WB_latch.inst.valid && MEM_WB_latch.inst.rd == ex_latch->inst.rs2) {
             val2 = MEM_WB_latch.alu_result; // Forward from WB
        }
    }
    
    ex_latch->val_rs1 = val1;
    ex_latch->val_rs2 = val2;

    switch (ex_latch->inst.op) {
        case OP_MOV: ex_latch->alu_result = ex_latch->inst.imm; break;
        case OP_ADD: ex_latch->alu_result = val1 + val2; break;
        case OP_SUB: ex_latch->alu_result = val1 - val2; break;
        case OP_MUL: ex_latch->alu_result = val1 * val2; break;
        default: ex_latch->alu_result = 0; break;
    }
}

/**
 * @brief Stage 4: Memory Access
 * In this simplified architecture, no instructions access memory.
 * This stage is a placeholder and simply passes data through.
 */
void mem_stage() {
    // No operation needed for this ISA. Data flows from EX_MEM to MEM_WB latch.
}

/**
 * @brief Stage 5: Write Back
 * Writes the result from the MEM_WB latch back to the register file.
 */
void wb_stage() {
    if (MEM_WB_latch.inst.valid && MEM_WB_latch.inst.op != OP_NOOP && MEM_WB_latch.inst.rd != -1) {
        R[MEM_WB_latch.inst.rd] = MEM_WB_latch.alu_result;
    }
}


// ---- Simulation and Printing ----

void print_stage_details(const char *name, const StageLatch *s) {
    if (!s->inst.valid || s->inst.op == OP_NOOP) {
        printf("%-6s: %-20s ", name, "NOP");
        return;
    }
    printf("%-6s: %-20s", name, s->inst.text);
}

void print_cycle_state(int cycle) {
    printf("\n================ Cycle %d ================\n", cycle);
    
    // Show what's in each pipeline latch
    if (PC < inst_count) printf("IF    : Fetching '%s'\n", program[PC].text);
    else printf("IF    : Done\n");

    print_stage_details("ID", &IF_ID_latch); printf("\n");

    if (!ID_EX_latch.inst.valid || ID_EX_latch.inst.op == OP_NOOP) {
        printf("EX    : NOP\n");
    } else if (ID_EX_latch.inst.op == OP_MOV) {
        printf("EX    : %-20s (result=%d)\n", ID_EX_latch.inst.text, ID_EX_latch.alu_result);
    } else {
        printf("EX    : %-20s (vals: %d, %d; result=%d)\n",
               ID_EX_latch.inst.text, ID_EX_latch.val_rs1, ID_EX_latch.val_rs2, ID_EX_latch.alu_result);
    }

    print_stage_details("MEM", &EX_MEM_latch); printf("\n");
    print_stage_details("WB", &MEM_WB_latch); printf("\n");

    // Print register file
    printf("\nRegisters: ");
    for (int i = 0; i < NUM_REGS; ++i) {
        printf("R%-2d=%-5d ", i, R[i]);
        if ((i + 1) % 8 == 0) printf("\n           ");
    }
    printf("\n");
}


int main() {
    // Initialize register file
    for (int i = 0; i < NUM_REGS; ++i) R[i] = 0;

    // Load program from file
    if (program_load("inst.txt") != 0) {
        fprintf(stderr, "Could not open inst.txt. Please create it.\n");
        return 1;
    }

    init_pipeline();
    int cycle = 1;

    // Main simulation loop: runs until the program is fetched and the pipeline is empty.
    while (PC < inst_count || !pipeline_is_empty()) {
        
        // --- Part 1: Execute stages for the current cycle state ---
        // These run "in parallel". We run them backwards to ensure
        // that stages read values from latches before they are overwritten.
        wb_stage();
        mem_stage();
        execute_stage();
        bool needs_stall = decode_stage();
        
        // --- Part 2: Print the state of the pipeline at the end of the current cycle ---
        print_cycle_state(cycle);

        // --- Part 3: Advance the pipeline for the NEXT cycle ---
        
        // MEM -> WB
        MEM_WB_latch = EX_MEM_latch;

        // EX -> MEM
        EX_MEM_latch = ID_EX_latch;

        // ID -> EX
        if (needs_stall) {
            // Stall detected! Insert a NOP (bubble) into the EX stage.
            ID_EX_latch.inst = make_nop();
        } else {
            ID_EX_latch = IF_ID_latch;
        }

        // IF -> ID
        if (needs_stall) {
            // Do nothing. The IF_ID latch keeps its current instruction,
            // and the PC is not advanced, causing a re-fetch next cycle.
        } else {
            Instruction fetched_inst;
            fetch_stage(&fetched_inst);
            IF_ID_latch.inst = fetched_inst;
            PC++; // Advance PC only if not stalling
        }

        cycle++;
    }

    // Final summary
    printf("\n=============== FINAL REGISTER STATE ===============\n");
    for (int i = 0; i < NUM_REGS; ++i) {
        printf("R%-2d=%-5d ", i, R[i]);
        if ((i + 1) % 8 == 0) printf("\n");
    }
    printf("\nTotal cycles: %d\n", cycle - 1);

    return 0;
}