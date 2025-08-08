
# Simple 5-Stage Pipeline Simulation in C

This project simulates a basic **5-stage instruction pipeline** (IF → ID → EX → MEM → WB) with **RAW hazard handling**.  
It reads instructions from a `instructions.txt` file, executes them with pipeline stalls if hazards are detected,  
and outputs the final register values, number of stalls, and total cycles into `output.txt`.

---

## Files
- `prog1.c` — C program for pipeline simulation.
- `instructions.txt` — Input file containing instructions.
- `output.txt` — Output file generated after execution.

1. **Compile and run the program**

   gcc prog1.c -o prog1
   prog1.exe
 
