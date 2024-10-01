// Libc
#include <stdbool.h>  // true, false
#include <stdint.h>   // uint16_t, etc
#include <stdio.h>    // printf, FILE, etc
#include <stdlib.h>   // exit
// POSIX
#include <termios.h>  // struct termios, etc
#include <unistd.h>   // STDIN_FILENO

// Total amount of words in memory
#define MEMORY_SIZE 0x10000L

// Sanity check for functions
// If condition fails then the program is incorrect and should exit
#define assert(_condition, ...)                                      \
    {                                                                \
        if (!(_condition)) {                                         \
            fprintf(stderr, "Assertion failed:\n" #_condition "\n"); \
            fprintf(stderr, "" __VA_ARGS__);                         \
            fprintf(stderr, "\n");                                   \
            exit(ERR_ASSERT);                                        \
        }                                                            \
    }

// 1 Word = 2 Bytes
typedef uint16_t Word;
typedef int16_t SignedWord;

// All program state
static Word memory[MEMORY_SIZE];
static Word registers[8];  // General purpose registers
static Word pc;            // Program counter
static uint8_t cc;         // Condition code

// All opcodes. Note that some refer to multiple instruction names
enum Opcode {
    OP_BR = 0x0,  // For all BR[nzp] instructions
    OP_ADD = 0x1,
    OP_LD = 0x2,
    OP_ST = 0x3,
    OP_JSR_JSRR = 0x4,  // Bitflag determines immediate or register
    OP_AND = 0x5,
    OP_LDR = 0x6,
    OP_STR = 0x7,
    OP_RTI = 0x8,  // Not used in non-supervisor mode
    OP_NOT = 0x9,
    OP_LDI = 0xa,
    OP_STI = 0xb,
    OP_JMP_RET = 0xc,   // RET == JMP R7
    OP_RESERVED = 0xd,  // Reserved instruction
    OP_LEA = 0xe,
    OP_TRAP = 0xf,
};

// All trap vectors
enum TrapVect {
    TRAP_GETC = 0x20,
    TRAP_OUT = 0x21,
    TRAP_PUTS = 0x22,
    TRAP_IN = 0x23,
    TRAP_PUTSP = 0x24,
    TRAP_HALT = 0x25,
};

// Kinds of user errors
enum Error {
    ERR_OK,           // Halted successfully
    ERR_CLI,          // Parsing command-line arguments
    ERR_FILE,         // Opening/reading file, invalid file structure
    ERR_INSTRUCTION,  // Invalid instruction or padding
    ERR_ASSERT,       // Assertion failed; this codebase has a bug
};

// Swap high and low bytes of a word
// 0x12ab -> 0xab12
// Object file is stored in different 'endianess' to program memory
Word swap_endian(const Word word) {
    return (word << 8) | (word >> 8);
}

// Set the condition code based on the value stored into a register
void set_cc(const SignedWord result) {
    if (result < 0) {
        cc = 0x4;  // Negative
    } else if (result == 0) {
        cc = 0x2;  // Zero
    } else {
        cc = 0x1;  // Positive
    }
}

// Sign extend a number to be a valid signed word
// This just makes the number valid in 2's compliment, at a larger size
SignedWord sign_extend(const Word value, const uint8_t bits) {
    Word sign_bit = 1 << (bits - 1);
    return (SignedWord)(value ^ sign_bit) - (SignedWord)sign_bit;
}
// Get bits highest to lowest (both inclusive)
// Note that bits ordered low to high, from 0
Word bits(const Word instruction, const uint8_t highest, const uint8_t lowest) {
    assert(highest >= lowest, "%d >= %d", highest, lowest);
    return (instruction >> lowest) & ((1 << (highest - lowest + 1)) - 1);
}
// Get bits and sign extend
SignedWord bits_sext(
    const Word instruction, const uint8_t highest, const uint8_t lowest
) {
    return sign_extend(
        bits(instruction, highest, lowest), highest - lowest + 1
    );
}

// Alias functions for common `bits` calls, for readability
uint8_t bits_reg_a(Word instruction) {
    return bits(instruction, 11, 9);
}
uint8_t bits_reg_b(Word instruction) {
    return bits(instruction, 8, 6);
}
uint8_t bits_reg_c(Word instruction) {
    return bits(instruction, 2, 0);
}
SignedWord bits_imm_5(Word instruction) {
    return bits_sext(instruction, 4, 0);
}
SignedWord bits_offset_6(Word instruction) {
    return bits_sext(instruction, 5, 0);
}
SignedWord bits_pc_offset_9(Word instruction) {
    return bits_sext(instruction, 8, 0);
}
SignedWord bits_pc_offset_11(Word instruction) {
    return bits_sext(instruction, 10, 0);
}

// Helper functions to make sure some `IN` prompt is printed on it's own line
static bool stdout_on_new_line = true;
void print_char(const char ch) {
    printf("%c", ch);
    stdout_on_new_line = ch == '\n';
}
void print_on_new_line() {
    if (stdout_on_new_line)
        return;
    printf("\n");
    stdout_on_new_line = true;
}

// Don't worry about this. It's to disable line buffering for stdin.
void enable_raw_terminal() {
    struct termios tty;
    (void)tcgetattr(STDIN_FILENO, &tty);
    tty.c_lflag &= ~ICANON;
    tty.c_lflag &= ~ECHO;
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &tty);
}
void disable_raw_terminal() {
    struct termios tty;
    (void)tcgetattr(STDIN_FILENO, &tty);
    tty.c_lflag |= ICANON;
    tty.c_lflag |= ECHO;
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &tty);
}

int main(const int argc, const char *const *const argv) {
    // Invalid arguments
    if (argc != 2 || argv[1][0] == '-' || argv[1][0] == '\0') {
        fprintf(stderr, "Usage: minilc3 [FILE]\n");
        return ERR_CLI;
    }

    // Try to open file
    FILE *const file = fopen(argv[1], "rb");
    size_t words_read;
    if (file == NULL) {
        fprintf(stderr, "Failed to open file.\n");
        return ERR_FILE;
    }

    // Read the first word: the memory origin
    Word origin;
    words_read = fread(&origin, sizeof(Word), 1, file);
    if (ferror(file)) {
        fprintf(stderr, "Failed to read file.");
        (void)fclose(file);
        return ERR_FILE;
    }
    if (words_read < 1) {
        fprintf(stderr, "File is too short.");
        (void)fclose(file);
        return ERR_FILE;
    }
    origin = swap_endian(origin);  // Fix endianess

    // Read the rest of the file into memory
    words_read =
        fread(memory + origin, sizeof(Word), MEMORY_SIZE - origin, file);
    if (ferror(file)) {
        fprintf(stderr, "Failed to read file.");
        (void)fclose(file);
        return ERR_FILE;
    }
    if (!feof(file)) {
        fprintf(stderr, "File is too long.");
        (void)fclose(file);
        return ERR_FILE;
    }
    (void)fclose(file);  // Close file
    if (words_read == 0) {
        fprintf(stderr, "File is too short.");
        return ERR_FILE;
    }

    // Fix endianess
    for (size_t i = origin; i < origin + words_read; ++i)
        memory[i] = swap_endian(memory[i]);

    // Reset registers
    pc = origin;
    cc = 0x2;  // Zero flag
    for (int i = 0; i < 8; ++i)
        registers[i] = 0;

    // See LC-3 instruction set for details on how instructions are layed
    // out in binary.

    // General layout of instructions (bits ordered low to high, from 0):
    // * Bits 12-15: Opcode
    // * Bits 9-11: Destination register (DR) or condition code for BR[nzp]
    // * Bits 6-8: Source register 1 (SR1) or base register (BaseR)
    // * Remaining low bits: Immediate (imm5), PC offset
    //     (PCoffset9/PCoffset11), base offset (offset6), trap vector
    //     (trapvect8)

    // Some instructions can have single-bit 'flags' to indicate whether some
    // bits refer to a register or an immediate (ADD, AND, JSR/JSRR).

    // Some instructions use the same opcode, or are aliases of other
    // instructions. Eg. RET is JMP R7, and JSR and JSRR use the same opcode,
    // with a flag.

    // Instructions can have padding of 0's (or 1's for NOT) which can be
    // ignored, but is checked here anyway.

    while (true) {
        // Get next instruction, then increment PC
        const Word instruction = memory[pc++];
        const enum Opcode opcode = (enum Opcode)bits(instruction, 15, 12);

        switch (opcode) {
            // ADD*
            case OP_ADD: {
                const uint8_t dest_reg = bits_reg_a(instruction);
                const uint8_t src_reg = bits_reg_b(instruction);
                SignedWord second_operand;
                if (bits(instruction, 5, 5) == 0) {
                    // Second operand is a register
                    if (bits(instruction, 4, 3) != 0) {
                        fprintf(stderr, "Invalid padding for ADD\n");
                        return ERR_INSTRUCTION;
                    }
                    second_operand =
                        (SignedWord)registers[bits_reg_c(instruction)];
                } else {
                    // Second operand is an immediate
                    second_operand = bits_imm_5(instruction);
                }
                const SignedWord result =
                    (SignedWord)registers[src_reg] + second_operand;
                registers[dest_reg] = (Word)result;
                set_cc(result);
            } break;

            // AND*
            case OP_AND: {
                const uint8_t dest_reg = bits_reg_a(instruction);
                const uint8_t src_reg = bits_reg_b(instruction);
                Word second_operand;
                if (bits(instruction, 5, 5) == 0) {
                    // Second operand is a register
                    if (bits(instruction, 4, 3) != 0) {
                        fprintf(stderr, "Invalid padding for ADD\n");
                        return ERR_INSTRUCTION;
                    }
                    second_operand = registers[bits_reg_c(instruction)];
                } else {
                    // Second operand is an immediate
                    second_operand = (Word)bits_imm_5(instruction);
                }
                const Word result = registers[src_reg] & second_operand;
                registers[dest_reg] = result;
                set_cc((SignedWord)result);
            } break;

            // NOT*
            case OP_NOT: {
                const uint8_t dest_reg = bits_reg_a(instruction);
                const uint8_t src_reg = bits_reg_b(instruction);
                if (bits(instruction, 5, 0) != 0x3f) {
                    fprintf(stderr, "Invalid padding for NOT\n");
                    return ERR_INSTRUCTION;
                }
                const Word result = ~registers[src_reg];
                registers[dest_reg] = result;
                set_cc((SignedWord)result);
            } break;

            // LEA*
            case OP_LEA: {
                const uint8_t dest_reg = bits_reg_a(instruction);
                const SignedWord pc_offset = bits_pc_offset_9(instruction);
                registers[dest_reg] = pc + pc_offset;
            } break;

            // LD*
            case OP_LD: {
                const uint8_t dest_reg = bits_reg_a(instruction);
                const SignedWord pc_offset = bits_pc_offset_9(instruction);
                const Word result = memory[pc + pc_offset];
                registers[dest_reg] = result;
                set_cc((SignedWord)result);
            } break;

            // LDI*
            case OP_LDI: {
                const uint8_t dest_reg = bits_reg_a(instruction);
                const SignedWord pc_offset = bits_pc_offset_9(instruction);
                const Word address = memory[pc + pc_offset];
                const Word result = memory[address];
                registers[dest_reg] = result;
                set_cc((SignedWord)result);
            } break;

            // LDR*
            case OP_LDR: {
                const uint8_t dest_reg = bits_reg_a(instruction);
                const uint8_t base_reg = bits_reg_b(instruction);
                const SignedWord offset = bits_offset_6(instruction);
                const Word result = memory[registers[base_reg] + offset];
                registers[dest_reg] = result;
                set_cc((SignedWord)result);
            } break;

            // ST
            case OP_ST: {
                const uint8_t src_reg = bits_reg_a(instruction);
                const SignedWord pc_offset = bits_pc_offset_9(instruction);
                const Word result = registers[src_reg];
                memory[pc + pc_offset] = result;
            } break;

            // STI
            case OP_STI: {
                const uint8_t src_reg = bits_reg_a(instruction);
                const SignedWord pc_offset = bits_pc_offset_9(instruction);
                const Word address = memory[pc + pc_offset];
                const Word result = registers[src_reg];
                memory[address] = result;
            } break;

            // STR
            case OP_STR: {
                const uint8_t src_reg = bits_reg_a(instruction);
                const uint8_t base_reg = bits_reg_b(instruction);
                const SignedWord offset = bits_offset_6(instruction);
                const Word result = registers[src_reg];
                memory[registers[base_reg] + offset] = result;
            } break;

            // BR[nzp]
            case OP_BR: {
                // Skip NOP case
                if (instruction == 0x0000)
                    continue;
                const uint8_t condition = bits_reg_a(instruction);
                // Cannot have no flags. `BR` is assembled as `BRnzp`
                if (condition == 0) {
                    fprintf(stderr, "Invalid condition for BR[nzp]\n");
                    return ERR_INSTRUCTION;
                }
                const SignedWord pc_offset = bits_pc_offset_9(instruction);
                if (cc & condition)
                    pc += pc_offset;
            } break;

            // JMP/RET
            case OP_JMP_RET: {
                if (bits(instruction, 11, 9) != 0 ||
                    bits(instruction, 5, 0) != 0) {
                    fprintf(stderr, "Invalid padding for JMP/RET\n");
                    return ERR_INSTRUCTION;
                }
                const uint8_t base_reg = bits_reg_b(instruction);
                pc = registers[base_reg];
            } break;

            // JSR/JSRR
            case OP_JSR_JSRR: {
                registers[7] = pc;
                if (bits(instruction, 11, 11)) {
                    // JSR
                    const SignedWord pc_offset = bits_pc_offset_11(instruction);
                    pc += pc_offset;
                } else {
                    // JSRR
                    if (bits(instruction, 11, 9) != 0 ||
                        bits(instruction, 5, 0) != 0) {
                        fprintf(stderr, "Invalid padding for JSRR\n");
                        return ERR_INSTRUCTION;
                    }
                    const uint8_t base_reg = bits_reg_b(instruction);
                    pc = registers[base_reg];
                }
            } break;

            // TRAP
            case OP_TRAP: {
                if (bits(instruction, 11, 8) != 0) {
                    fprintf(stderr, "Invalid padding for TRAP\n");
                    return ERR_INSTRUCTION;
                }
                const enum TrapVect trap_vect =
                    (enum TrapVect)bits(instruction, 8, 0);
                switch (trap_vect) {
                    // GETC
                    case TRAP_GETC: {
                        enable_raw_terminal();
                        const char input = (char)getchar();
                        disable_raw_terminal();
                        registers[0] = (Word)input;
                    }; break;

                    // IN
                    case TRAP_IN: {
                        print_on_new_line();
                        printf("Input> ");
                        enable_raw_terminal();
                        const char input = (char)getchar();
                        disable_raw_terminal();
                        print_char(input);
                        print_on_new_line();
                        registers[0] = (Word)input;
                    }; break;

                    // OUT
                    case TRAP_OUT: {
                        print_char((char)(registers[0]));
                        (void)fflush(stdout);
                    }; break;

                    // PUTS
                    case TRAP_PUTS: {
                        for (Word i = registers[0];; ++i) {
                            const char ch = (char)(memory[i]);
                            if (ch == '\0')
                                break;
                            print_char(ch);
                        }
                        (void)fflush(stdout);
                    }; break;

                    // PUTSP
                    case TRAP_PUTSP: {
                        for (Word i = registers[0];; ++i) {
                            const Word word = memory[i];
                            const char chars[2] = {
                                (char)(word >> 8), (char)word
                            };

                            if (chars[0] == '\0')
                                break;
                            print_char(chars[0]);
                            if (chars[1] == '\0')
                                break;
                            print_char(chars[1]);
                        }
                        (void)fflush(stdout);
                    }; break;

                    // HALT
                    case TRAP_HALT:
                        goto halt;

                    // Could be a non-standard trap, so not unreachable
                    default:
                        fprintf(
                            stderr, "Invalid TRAP vector 0x%02hhx\n", trap_vect
                        );
                        return ERR_INSTRUCTION;
                }
            } break;

            // RTI
            case OP_RTI:
                fprintf(stderr, "Cannot use RTI in non-supervisor mode\n");
                return ERR_INSTRUCTION;
            // Reserved
            case OP_RESERVED:
                fprintf(stderr, "Cannot use reserved instruction\n");
                return ERR_INSTRUCTION;
        }
    }
halt:

    print_on_new_line();
    return ERR_OK;
}
