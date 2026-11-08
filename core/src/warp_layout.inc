// The warp image's layout (Phase 8, design.md 6.14): the constants, a body's
// record, block-table and page entries, codon-word bits, Op numbers and
// what each random draw is for. Shared by warp_isa.inc (both of its
// compilers) and the C++ that packs, unpacks and runs it (warp.cpp,
// accel_dx.cpp). Plain #defines only, so C++ and HLSL both read it.

// --- constants (CST) ------------------------------------------------------------
#define W_NONE 0xFFFFFFFFu
#define WC_TABLE 0u       // 64 words: Op per codon
#define WC_SENSE 64u      // 28 words: SENSE's selector, BX -> reading
#define WC_HEAD 92u       // 2 words: SETH/GETH's selector, BX -> head (0 read, 1 write)
#define WC_LANES 94u      // lanes per warp
#define WC_STACK 95u      // stack depth
#define WC_SEARCH 96u     // template search limit, each way
#define WC_TEMPLATE 97u   // longest template
#define WC_HALT 98u       // consecutive errors that halt a lane
#define WC_SENSE_N 99u    // SENSE's readings
#define WC_STEP_COST 100u // work per step
#define WC_LANE_COST 101u // work per lane in a step
#define WC_OPTIME 102u    // 96 words (Phase 9b): a step's time by its Op, then for a lane that
                          // can't run the code (all 0: no clock)
#define WC_SCAN 198u      // a place a lane's template search looks at (Phase 9c; 0: free)
#define WC_SLEEP 199u     // 1: YIELD is a host instruction, which may send the body to sleep
                          // (docs/lifehistory-plan.md V5, life.sleep); 0: a lane's, as before
#define WC_COUNT 200u

// --- a body's record in the image (offsets from the record) -------------------
#define WB_NBLK 0u        // blocks
#define WB_BLK 1u         // where its block table starts (WK_SIZE words a block)
#define WB_LO 2u          // the body's span: lowest block start
#define WB_HI 3u          //   ... and highest block end
#define WB_SIZE 4u        // the node's soup size
#define WB_KEY 5u         // this tick's random key
#define WB_COPY_THR 6u    // copy error: a draw below this
#define WB_FLAW_THR 7u    // flaw: a draw below this
#define WB_SALV_LIM 8u    // salvage: codons stamped above this may come out garbled
#define WB_STAMP 9u       // stamp word = codon word + this (0: no stamps)
#define WB_NWARP 10u      // warps to run
#define WB_SENSE_OK 11u   // bit r: SENSE reading r is available
#define WB_SENSE 12u      // 28 words: the readings, taken before the GPU phase
#define WB_OUT 40u        // outputs (WO_*)
#define WO_COPYERR 0u
#define WO_FLAWS 1u
#define WO_GARBLED 2u
#define WO_HALTS 3u
#define WO_WRITES 4u
#define WB_SORT 45u       // where the blocks' index starts: its pair count, then (start, entry) pairs
                          // (0: none, look them up in order; warp.cpp's warp_pack, w_range)
// Memory tiers (docs/world-plan.md W3), the record's node's: codons [0, WB_FAST)
// are fast memory (0: one tier, every step as before); then a lane's price
// of each in the fast tier and the slow one, already divided by the node's
// speeds: a load (COPY, READ, MOVII), a store (COPY, WRITE, MOVII), and a
// place or template codon searched.
#define WB_FAST 46u
#define WB_TLOAD 47u      // 2 words: fast, slow
#define WB_TSTORE 49u     // 2 words
#define WB_TSCAN 51u      // 2 words
#define WB_WARPS 53u      // per warp, WW_SIZE words
#define WW_LANE0 0u       // its first lane in LANES
#define WW_QUOTA 1u       // work it may do
#define WW_INDEX 2u       // its place in the body's list (its key)
#define WW_STEP0 3u       // steps it already ran this tick (earlier rounds)
#define WW_USED 4u        // out: work done
#define WW_STEPS 5u       // out: steps run
#define WW_LANEINS 6u     // out: lane instructions run
#define WW_WAIT 7u        // out: 1 = stopped in front of a host instruction
#define WW_WAITIP 8u      // out: ... at this IP
#define WW_LIVE 9u        // out: its lanes not halted
#define WW_ACTIVE 10u     // out: ... nor yielded
#define WW_TLEFT 11u      // its own clock this tick (Phase 9b): time it may still take (W_NONE: none)
#define WW_TUSED 12u      // out: time it took
#define WW_SIZE 13u
// block table entry
#define WK_START 0u
#define WK_LEN 1u
#define WK_CODE 2u        // where its codon words start
#define WK_ID 3u          // the soup's block id
#define WK_PAGE 4u        // where its page entries start (WP_SIZE words a page)
#define WK_SIZE 5u
// page entry
#define WP_THR 0u         // a hot page accepts a write if a draw is below this (W_NONE: always)
#define WP_HEAT 1u        // its heat, as SENSE reads it
#define WP_FETCH 2u       // out: steps fetched from it
#define WP_WRITE 3u       // out: codons written into it
#define WP_SIZE 4u
// codon word
#define CW_CODON 63u
#define CW_MINE 128u      // owned by this body
#define CW_OWNER_SHIFT 8u // bits 8-23: the owning block's place in the block table
#define CW_WRITTEN 0x1000000u // written by a lane this round

// --- ops (nova::Op; warp.cpp checks every value) --------------------------------
#define OP_NOP0 0u
#define OP_NOP1 1u
#define OP_JMP 2u
#define OP_IFZ 3u
#define OP_INC 4u
#define OP_DEC 5u
#define OP_ZERO 6u
#define OP_ROT 7u
#define OP_PUSH 8u
#define OP_POP 9u
#define OP_COPY 10u
#define OP_FIND 11u
#define OP_IFLT 12u
#define OP_SENSE 13u
#define OP_ALLOC 14u
#define OP_DIVIDE 15u
#define OP_CALL 16u
#define OP_RET 17u
#define OP_DJNZ 18u
#define OP_JZ 19u
#define OP_ADD 20u
#define OP_SUB 21u
#define OP_MUL 22u
#define OP_DIV 23u
#define OP_AND 24u
#define OP_OR 25u
#define OP_XOR 26u
#define OP_NOT 27u
#define OP_SHL 28u
#define OP_SHR 29u
#define OP_NEG 30u
#define OP_RAND 31u
#define OP_IFNE 32u
#define OP_IFERR 33u
#define OP_SWAP 34u
#define OP_ROTB 35u
#define OP_SETH 36u
#define OP_GETH 37u
#define OP_READ 38u
#define OP_WRITE 39u
#define OP_ENGULF 40u
#define OP_INJECT 41u
#define OP_EMIT 42u
#define OP_LISTEN 43u
#define OP_DISK 44u
#define OP_GPU 45u
#define OP_YIELD 46u
#define OP_RELEASE 47u
#define OP_T_NOT0 48u
#define OP_T_SHL 49u
#define OP_T_ZERO 50u
#define OP_T_IFZ 51u
#define OP_T_SUBCAB 52u
#define OP_T_SUBAAC 53u
#define OP_T_INCA 54u
#define OP_T_INCB 55u
#define OP_T_DECC 56u
#define OP_T_INCC 57u
#define OP_T_PUSHAX 58u
#define OP_T_PUSHBX 59u
#define OP_T_PUSHCX 60u
#define OP_T_PUSHDX 61u
#define OP_T_POPAX 62u
#define OP_T_POPBX 63u
#define OP_T_POPCX 64u
#define OP_T_POPDX 65u
#define OP_T_JMPO 66u
#define OP_T_JMPB 67u
#define OP_T_CALL 68u
#define OP_T_RET 69u
#define OP_T_MOVDC 70u
#define OP_T_MOVBA 71u
#define OP_T_MOVII 72u
#define OP_T_ADRO 73u
#define OP_T_ADRB 74u
#define OP_T_ADRF 75u
#define OP_T_MAL 76u
#define OP_T_DIVIDE 77u
#define OP_NOPX 78u
#define OP_JMPF 79u
#define OP_JMPB 80u
#define OP_IFEQ 81u
#define OP_IFGT 82u
#define OP_MOD 83u
#define OP_MIN 84u
#define OP_MAX 85u
#define OP_DUP 86u
#define OP_DROP 87u
#define OP_INCA 88u
#define OP_DECA 89u
#define OP_INCB 90u
#define OP_DECB 91u
#define OP_ZEROA 92u
#define OP_ZEROB 93u
#define OP_INERT 94u      // Phase 10: dead matter; a lane running it fails (the default case)
#define OP_COUNT 95u

// what a random draw is for
#define WD_FLAW 1u
#define WD_FLAW_KIND 2u
#define WD_COPY 3u
#define WD_COPY_BIT 4u
#define WD_PAGE 5u
#define WD_SALV 6u
#define WD_SALV_V 7u
#define WD_RAND 8u

