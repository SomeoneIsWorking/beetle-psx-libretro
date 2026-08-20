#ifndef PSXPORT_GPU_CENSUS_H
#define PSXPORT_GPU_CENSUS_H
/* psxport: a COMMAND CENSUS for the beetle GPU oracle (runtime/recomp/gpu_beetle.cpp).
 *
 * WHY. The oracle tees every GP0/GP1 word into beetle and then diffs the two VRAMs. A difference is
 * only evidence about RASTERIZATION once the FEED is known to be complete -- otherwise "beetle did
 * not draw the wood panel" is indistinguishable from "beetle was never asked to draw the wood
 * panel", and the second is what actually happened the first two times this was wired up.
 *
 * The tee side CANNOT compute this. It sees a stream of 32-bit words with no command boundaries; the
 * per-opcode length table (Commands[256]) lives here, inside beetle, and the FIFO that consumes them
 * is here too. Counting "commands beetle executed" is therefore only possible from inside gpu.c.
 *
 * THE NEGATIVE IS THE POINT (CLAUDE.md: a diagnostic that can print nothing is lying). Every counter
 * below is a DENOMINATOR or a LOSS channel, so a frame in which beetle drew nothing prints why:
 * words never accepted (FIFO full), commands never dispatched (draw-time starved), opcodes beetle
 * treats as no-ops, commands dispatched to a NULL rasteriser specialisation, and words still sitting
 * unconsumed in the FIFO when the frame ended. "poly=0" alone would be silence with a number on it.
 *
 * Always compiled, not behind PSXPORT_HOOKS: it is a handful of increments, and it costs literally
 * nothing when the oracle is off because nothing then drives GPU_Write/GPU_Update at all. A counter
 * you have to remember to switch on is a counter that is off when you need it.
 */

enum {
   PGC_WORDS_ACCEPTED = 0, /* GP0 words written into the FIFO                                    */
   PGC_WORDS_DROPPED,      /* GP0 words DISCARDED because the FIFO was full. On real hardware the
                              CPU stalls; beetle just drops them, so this is silent data loss and
                              the single most likely way the tee under-feeds the oracle.          */
   PGC_CMDS_DISPATCHED,    /* commands whose operands were pulled from the FIFO (the denominator) */
   PGC_POLY,               /* 0x20..0x3F dispatched as a NEW command                              */
   PGC_POLY_CONT,          /* a 4-point polygon's CONTINUATION packet: beetle rasterises a quad as
                              two triangles and dispatches the second separately, so counting it as
                              a primitive double-counts every quad. Measured: 390 "polys" for 195
                              real ones, which read as a lossy feed until it was split out.        */
   PGC_LINE,               /* 0x40..0x5F dispatched as a NEW command                              */
   PGC_LINE_CONT,          /* a POLYLINE's continuation packet. Same trap as PGC_POLY_CONT: beetle
                              dispatches every segment of a polyline separately, so counting them as
                              primitives over-counts a producer that submits one polyline. Measured:
                              6 line dispatches for 2 polylines made a complete feed read as +4.    */
   PGC_SPRITE,             /* 0x60..0x7F dispatched                                              */
   PGC_XFER,               /* 0x80..0xDF dispatched (FBCopy / FBWrite / FBRead)                  */
   PGC_FILL,               /* 0x02 FBFill                                                        */
   PGC_STATE,              /* 0xE1..0xE6 draw-mode / clip / offset / mask                         */
   PGC_NOP0,               /* opcodes 0x00 (NOP) and 0x01 (Clear Cache) -- LEGITIMATE no-ops. Games
                              pad OTs with 0x00 and issue 0x01 during normal rendering, so neither
                              may be confused with a word beetle failed to understand.             */
   PGC_NOP,                /* an opcode beetle has NO command for, EXCLUDING 0x00/0x01 -- a real
                              primitive landing here means the tee mangled the word, not that the
                              game drew nothing.                                                  */
   PGC_NOP_LAST,           /* the most recent such opcode, so the warning can NAME it rather than
                              just count it (a count alone cannot be acted on).                   */
   PGC_NULL_FUNC,          /* a DRAWING opcode (0x20..0x7F) dispatched, but func[abr][TexMode] was
                              NULL: accepted and then not rasterised. Restricted to drawing opcodes
                              on purpose -- opcode 0x00 has an all-NULL matrix by design, so
                              counting it here made every frame report 55 phantom losses.         */
   PGC_STARVED,            /* ProcessFIFO returned early with DrawTimeAvail < 0, i.e. a command
                              was ready and beetle refused to run it yet.                         */
   PGC_N
};

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned long psxport_gpu_census[PGC_N];
extern unsigned long psxport_gpu_fifo_depth(void); /* words queued but not yet consumed */
extern void psxport_gpu_grant_drawtime(void);      /* oracle only: remove the draw-time budget */
extern int  psxport_gpu_selftest_bias;             /* oracle self-test: px to shift every primitive */
#ifdef __cplusplus
}
#endif

#endif
