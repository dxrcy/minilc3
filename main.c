#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

#define MEMORY_SIZE 0x10000L

typedef uint16_t Word;
typedef int16_t SignedWord;

static Word memory[MEMORY_SIZE];
static Word registers[8];
static Word pc;
static uint8_t cc;

enum Error {
    ERR_CLI,
    ERR_FILE,
    ERR_INSTRUCTION,
    ERR_UNREACHABLE,
};

Word swap_endian(const Word word) {
    return (word << 8) | (word >> 8);
}

void set_cc(const SignedWord result) {
    if (result & 0x8000) {
        cc = 0x4;
    } else if (result == 0) {
        cc = 0x2;
    } else {
        cc = 0x1;
    }
}

SignedWord sign_extend(const Word value, const uint8_t bits) {
    Word sign_bit = 1 << (bits - 1);
    return (SignedWord)(value ^ sign_bit) - (SignedWord)sign_bit;
}

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

static bool stdout_on_new_line = true;
void print_char(char ch) {
    printf("%c", ch);
    stdout_on_new_line = ch == '\n';
}
void print_on_new_line() {
    if (stdout_on_new_line)
        return;
    printf("\n");
    stdout_on_new_line = true;
}

int main(const int argc, const char *const *const argv) {
    if (argc != 2 || argv[1][0] == '-' || argv[1][0] == '\0') {
        fprintf(stderr, "Usage: minilc3 [FILE]\n");
        return ERR_CLI;
    }

    FILE *const file = fopen(argv[1], "rb");
    size_t words_read;
    if (file == NULL) {
        fprintf(stderr, "Failed to open file.\n");
        return ERR_FILE;
    }

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
    origin = swap_endian(origin);

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
    (void)fclose(file);
    if (words_read == 0) {
        fprintf(stderr, "File is too short.");
        return ERR_FILE;
    }

    for (size_t i = origin; i < origin + words_read; ++i)
        memory[i] = swap_endian(memory[i]);

    pc = origin;
    cc = 0x2;
    for (int i = 0; i < 8; ++i)
        registers[i] = 0;

    while (true) {
        const Word instruction = memory[pc++];
        const uint8_t opcode = instruction >> 12;

        switch (opcode) {
            // ADD*
            case 0x1: {
                const uint8_t dest_reg = (instruction >> 9) & 0x7;
                const uint8_t src_reg = (instruction >> 6) & 0x7;
                SignedWord value;
                if ((instruction & 0x20) == 0) {
                    if (((instruction >> 3) & 0x3) != 0) {
                        fprintf(stderr, "Invalid padding for ADD\n");
                        return ERR_INSTRUCTION;
                    }
                    value = (SignedWord)registers[instruction & 0x7];
                } else {
                    value = sign_extend(instruction & 0x1f, 5);
                }
                const SignedWord result =
                    (SignedWord)registers[src_reg] + value;
                registers[dest_reg] = (Word)result;
                set_cc(result);
            } break;

            // AND*
            case 0x5: {
                const uint8_t dest_reg = (instruction >> 9) & 0x7;
                const uint8_t src_reg = (instruction >> 6) & 0x7;
                Word value;
                if (instruction & 0x20) {
                    value = instruction & 0x1f;
                } else {
                    if (((instruction >> 3) & 0x3) != 0) {
                        fprintf(stderr, "Invalid padding for AND\n");
                        return ERR_INSTRUCTION;
                    }
                    value = registers[instruction & 0x7];
                }
                const Word result = registers[src_reg] & value;
                registers[dest_reg] = result;
                set_cc((SignedWord)result);
            } break;

            // NOT*
            case 0x9: {
                const uint8_t dest_reg = (instruction >> 9) & 0x7;
                const uint8_t src_reg = (instruction >> 6) & 0x7;
                if (~(instruction & 0x3f) != 0) {
                    fprintf(stderr, "Invalid padding for NOT\n");
                    return ERR_INSTRUCTION;
                }
                const Word result = ~registers[src_reg];
                registers[dest_reg] = result;
                set_cc((SignedWord)result);
            } break;

            // LEA*
            case 0xe: {
                const uint8_t dest_reg = (instruction >> 9) & 0x7;
                const SignedWord pc_offset =
                    sign_extend(instruction & 0x1ff, 9);
                registers[dest_reg] = pc + pc_offset;
            } break;

            // LD*
            case 0x2: {
                const uint8_t dest_reg = (instruction >> 9) & 0x7;
                const SignedWord pc_offset =
                    sign_extend(instruction & 0x1ff, 9);
                const Word result = memory[pc + pc_offset];
                registers[dest_reg] = result;
                set_cc((SignedWord)result);
            } break;

            // LDI*
            case 0xa: {
                const uint8_t dest_reg = (instruction >> 9) & 0x7;
                const SignedWord pc_offset =
                    sign_extend(instruction & 0x1ff, 9);
                const Word address = memory[pc + pc_offset];
                const Word result = memory[address];
                registers[dest_reg] = result;
                set_cc((SignedWord)result);
            } break;

            // LDR*
            case 0x6: {
                const uint8_t dest_reg = (instruction >> 9) & 0x7;
                const uint8_t base_reg = (instruction >> 6) & 0x7;
                const SignedWord offset = sign_extend(instruction & 0x3f, 6);
                const Word result = memory[registers[base_reg] + offset];
                registers[dest_reg] = result;
                set_cc((SignedWord)result);
            } break;

            // ST
            case 0x3: {
                const uint8_t src_reg = (instruction >> 9) & 0x7;
                const SignedWord pc_offset =
                    sign_extend(instruction & 0x1ff, 9);
                const Word result = registers[src_reg];
                memory[pc + pc_offset] = result;
            } break;

            // STI
            case 0xb: {
                const uint8_t src_reg = (instruction >> 9) & 0x7;
                const SignedWord pc_offset =
                    sign_extend(instruction & 0x1ff, 9);
                const Word address = memory[pc + pc_offset];
                const Word result = registers[src_reg];
                memory[address] = result;
            } break;

            // STR
            case 0x7: {
                const uint8_t src_reg = (instruction >> 9) & 0x7;
                const uint8_t base_reg = (instruction >> 6) & 0x7;
                const SignedWord offset = sign_extend(instruction & 0x3f, 6);
                const Word result = registers[src_reg];
                memory[registers[base_reg] + offset] = result;
            } break;

            // BR[nzp]
            case 0x0: {
                const uint8_t condition = (instruction >> 9) & 0x7;
                if (condition == 0) {
                    fprintf(stderr, "Invalid condition for BR[nzp]\n");
                    return ERR_INSTRUCTION;
                }
                const SignedWord pc_offset =
                    sign_extend(instruction & 0x1ff, 9);
                if (cc & condition)
                    pc += pc_offset;
            } break;

            // JMP/RET
            case 0xc: {
                if (((instruction >> 9) & 0x7) != 0 ||
                    (instruction & 0x3f) != 0) {
                    fprintf(stderr, "Invalid padding for JMP/RET\n");
                    return ERR_INSTRUCTION;
                }
                const uint8_t base_reg = (instruction >> 6) & 0x7;
                pc = registers[base_reg];
            } break;

            // JSR/JSRR
            case 0x4: {
                registers[7] = pc;
                if (instruction & 0x4ff) {
                    // JSR
                    const SignedWord pc_offset =
                        sign_extend(instruction & 0x7ff, 11);
                    pc += pc_offset;
                } else {
                    // JSRR
                    if (((instruction >> 9) & 0x7) != 0 ||
                        (instruction & 0x3f) != 0) {
                        fprintf(stderr, "Invalid padding for JSRR\n");
                        return ERR_INSTRUCTION;
                    }
                    const uint8_t base_reg = (instruction >> 6) & 0x7;
                    pc = registers[base_reg];
                }
            } break;

            // TRAP
            case 0xf: {
                if (((instruction >> 8) & 0xf) != 0) {
                    fprintf(stderr, "Invalid padding for TRAP\n");
                    return ERR_INSTRUCTION;
                }
                const uint8_t trap_vector = instruction & 0xff;
                switch (trap_vector) {
                    // GETC
                    case 0x20: {
                        enable_raw_terminal();
                        const char input = (char)getchar();
                        disable_raw_terminal();
                        registers[0] = (Word)input;
                    }; break;

                    // IN
                    case 0x23: {
                        print_on_new_line();
                        printf("Input> ");
                        enable_raw_terminal();
                        const char input = (char)getchar();
                        disable_raw_terminal();
                        print_char(input);
                        registers[0] = (Word)input;
                    }; break;

                    // OUT
                    case 0x21: {
                        print_char((char)(registers[0]));
                        (void)fflush(stdout);
                    }; break;

                    // PUTS
                    case 0x22: {
                        print_on_new_line();
                        for (Word i = registers[0];; ++i) {
                            const char ch = (char)(memory[i]);
                            if (ch == '\0')
                                break;
                            print_char(ch);
                        }
                        (void)fflush(stdout);
                    }; break;

                    // PUTSP
                    case 0x24: {
                        print_on_new_line();
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
                    case 0x25:
                        goto halt;

                    default:
                        fprintf(
                            stderr,
                            "Invalid TRAP vector 0x%02hhx\n",
                            trap_vector
                        );
                        return ERR_INSTRUCTION;
                }
            } break;

            // RTI
            case 0x8:
                fprintf(stderr, "Cannot use RTI in non-supervisor mode\n");
                return ERR_INSTRUCTION;
            // Reserved
            case 0xd:
                fprintf(stderr, "Cannot use reserved instruction\n");
                return ERR_INSTRUCTION;
            // Default branch should never be reached
            default:
                fprintf(stderr, "Unreachable code reached\n");
                return ERR_UNREACHABLE;
        }
    }
halt:

    print_on_new_line();
    return 0;
}

