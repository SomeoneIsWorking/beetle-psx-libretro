/* gpu_defer.c -- deferred pixels: the GPU the guest talks to keeps every register and command
 * exactly as before but draws nothing; each command it runs is logged, and a second PS_GPU sharing
 * its vram replays the log in order when VRAM is read (GPU_SettlePixels). A drawing command whose
 * pixels a later fill overwrites before anything reads them replays without pixels: its state effects
 * (texture page, CLUT cache, quad and polyline continuation) still happen, so the replay stays in step.
 *
 * #included by gpu.c ahead of the command handlers that note into it; the replay, which needs the
 * command table, is gpu_defer_replay.c.
 * Coordinates are VRAM pixels at upscale 0; a front end that defers runs gpu.c at native scale. */

/* A rect on the 1024x512 VRAM torus: x in [0,1024), y in [0,512), w in [0,1024], h in [0,512]. */
typedef struct
{
   int16_t x, y, w, h;
} DeferRect;

enum
{
   DEFER_COMMAND,   /* one dispatched GP0 command: its words in the pool */
   DEFER_UPLOAD,    /* upload words for the FBWrite in progress */
   DEFER_PLINE_END  /* the polyline terminator, which ends INCMD_PLINE without a dispatch */
};

#define DEFER_MAX_READS 3
#define DEFER_READS_ALL 0xFF

typedef struct
{
   uint8_t kind;
   uint8_t cc;
   uint8_t read_fifo;
   uint8_t dead;
   uint8_t drawing;   /* a draw or fill: the only kinds a later fill may make dead */
   uint8_t killer;    /* a fill that overwrites its whole rect, so earlier draws inside it can die */
   uint8_t nreads;    /* DEFER_READS_ALL: more than fit, treated as reading everything */
   uint8_t has_write;
   /* LineSkipTest's inputs as they were when the command ran; GPU_Update moves them on. */
   uint32_t display_mode;
   uint32_t display_ystart;
   bool field_ram_readout;
   DeferRect write;
   DeferRect reads[DEFER_MAX_READS];
   uint32_t words;    /* offset into the pool */
   uint32_t count;
} DeferEntry;

static struct
{
   bool on;
   PS_GPU *pixels;          /* the replaying instance */
   DeferEntry *entries;
   uint32_t nentries, entries_cap;
   uint32_t *pool;
   uint32_t npool, pool_cap;
   uint32_t dirty[32];      /* 32x16-pixel tiles a pending command may write: bit tx of row ty */
   bool any_dirty;
   bool settling;
   /* The command running now on the logging GPU. */
   DeferRect note_write;
   bool note_has_write;
   bool note_overflow;
   DeferRect note_reads[DEFER_MAX_READS];
   uint8_t note_nreads;
   bool upload_open;        /* the last entry takes further upload words */
} Defer;

/* Over this many pending entries or words the log is replayed whole, so it stays bounded. */
#define DEFER_ENTRY_LIMIT (1u << 16)
#define DEFER_POOL_LIMIT  (1u << 22)

static DeferRect DeferMakeRect(int32_t x, int32_t y, int32_t w, int32_t h)
{
   DeferRect r;
   if (w > 1024) w = 1024;
   if (h > 512)  h = 512;
   if (w < 0) w = 0;
   if (h < 0) h = 0;
   r.x = (int16_t)(x & 1023);
   r.y = (int16_t)(y & 511);
   r.w = (int16_t)w;
   r.h = (int16_t)h;
   return r;
}

/* The rect's pieces that do not wrap, half open {x0, y0, x1, y1}: at most four. */
static unsigned DeferSplit(DeferRect r, int32_t out[4][4])
{
   int32_t xs[2][2], ys[2][2];
   unsigned nx = 1, ny = 1, n = 0, i, j;
   if (r.w <= 0 || r.h <= 0)
      return 0;
   xs[0][0] = r.x; xs[0][1] = r.x + r.w;
   if (xs[0][1] > 1024) { xs[1][0] = 0; xs[1][1] = xs[0][1] - 1024; xs[0][1] = 1024; nx = 2; }
   ys[0][0] = r.y; ys[0][1] = r.y + r.h;
   if (ys[0][1] > 512) { ys[1][0] = 0; ys[1][1] = ys[0][1] - 512; ys[0][1] = 512; ny = 2; }
   for (j = 0; j < ny; j++)
      for (i = 0; i < nx; i++)
      {
         out[n][0] = xs[i][0]; out[n][1] = ys[j][0];
         out[n][2] = xs[i][1]; out[n][3] = ys[j][1];
         n++;
      }
   return n;
}

static bool DeferOverlap(DeferRect a, DeferRect b)
{
   int32_t pa[4][4], pb[4][4];
   unsigned na = DeferSplit(a, pa), nb = DeferSplit(b, pb), i, j;
   for (i = 0; i < na; i++)
      for (j = 0; j < nb; j++)
         if (pa[i][0] < pb[j][2] && pb[j][0] < pa[i][2] && pa[i][1] < pb[j][3] && pb[j][1] < pa[i][3])
            return true;
   return false;
}

static bool DeferWraps(DeferRect r)
{
   return r.x + r.w > 1024 || r.y + r.h > 512;
}

/* `inner` inside `outer`; neither wraps. */
static bool DeferContains(DeferRect outer, DeferRect inner)
{
   return inner.x >= outer.x && inner.y >= outer.y
       && inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}

/* Marks the rect's 32x16-pixel tiles in `tiles` (bit tx of row ty), or (mark false) says whether any is set. */
static bool DeferTileMap(uint32_t tiles[32], DeferRect r, bool mark)
{
   int32_t parts[4][4];
   unsigned n = DeferSplit(r, parts), p;
   for (p = 0; p < n; p++)
   {
      const int32_t tx0 = parts[p][0] >> 5, tx1 = (parts[p][2] + 31) >> 5;
      const int32_t ty0 = parts[p][1] >> 4, ty1 = (parts[p][3] + 15) >> 4;
      const uint32_t mask = (tx1 - tx0 >= 32) ? 0xFFFFFFFFu : (((1u << (tx1 - tx0)) - 1u) << tx0);
      int32_t ty;
      for (ty = ty0; ty < ty1; ty++)
      {
         if (mark)
            tiles[ty] |= mask;
         else if (tiles[ty] & mask)
            return true;
      }
   }
   return false;
}

static bool DeferTiles(DeferRect r, bool mark)
{
   return DeferTileMap(Defer.dirty, r, mark);
}

/* ---- notes, from the logging GPU's command handlers ---- */

static void DeferNoteWrite(DeferRect r)
{
   if (r.w <= 0 || r.h <= 0)
      return;
   if (Defer.note_has_write)
   {
      /* Two pieces of one command (the line-render pair): their bounds. Pieces of a draw never wrap. */
      const DeferRect a = Defer.note_write;
      const int32_t x0 = a.x < r.x ? a.x : r.x;
      const int32_t y0 = a.y < r.y ? a.y : r.y;
      const int32_t x1 = (a.x + a.w) > (r.x + r.w) ? (a.x + a.w) : (r.x + r.w);
      const int32_t y1 = (a.y + a.h) > (r.y + r.h) ? (a.y + a.h) : (r.y + r.h);
      r = DeferMakeRect(x0, y0, x1 - x0, y1 - y0);
   }
   Defer.note_write = r;
   Defer.note_has_write = true;
}

static void DeferNoteRead(DeferRect r)
{
   unsigned i;
   if (r.w <= 0 || r.h <= 0)
      return;
   for (i = 0; i < Defer.note_nreads; i++)
      if (memcmp(&Defer.note_reads[i], &r, sizeof r) == 0)
         return;
   if (Defer.note_nreads == DEFER_MAX_READS)
   {
      Defer.note_overflow = true;
      return;
   }
   Defer.note_reads[Defer.note_nreads++] = r;
}

/* A shape covering [x0, x1) x [y0, y1) before the drawing area clips it. A textured one reads its
 * texture page and CLUT; a blended or mask-tested one reads what it covers. */
static void DeferNoteDraw(PS_GPU *g, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
      bool textured, unsigned tex_mode, uint32_t clut_x, uint32_t clut_y, bool reads_destination)
{
   if (x0 < g->ClipX0) x0 = g->ClipX0;
   if (y0 < g->ClipY0) y0 = g->ClipY0;
   if (x1 > g->ClipX1 + 1) x1 = g->ClipX1 + 1;
   if (y1 > g->ClipY1 + 1) y1 = g->ClipY1 + 1;
   if (x0 < x1 && y0 < y1)
   {
      const DeferRect covered = DeferMakeRect(x0, y0, x1 - x0, y1 - y0);
      DeferNoteWrite(covered);
      if (reads_destination)
         DeferNoteRead(covered);
   }
   if (textured)
   {
      const int32_t page_width = tex_mode == 0 ? 64 : tex_mode == 1 ? 128 : 256;
      DeferNoteRead(DeferMakeRect((int32_t)g->TexPageX, (int32_t)g->TexPageY, page_width, 256));
      if (tex_mode < 2)
         DeferNoteRead(DeferMakeRect((int32_t)clut_x, (int32_t)clut_y, tex_mode ? 256 : 16, 1));
   }
}

static void DeferNoteTriangle(PS_GPU *g, const tri_vertex *v, bool textured, unsigned tex_mode,
      uint32_t clut_x, uint32_t clut_y, bool reads_destination)
{
   int32_t x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
   unsigned i;
   for (i = 1; i < 3; i++)
   {
      if (v[i].x < x0) x0 = v[i].x;
      if (v[i].x > x1) x1 = v[i].x;
      if (v[i].y < y0) y0 = v[i].y;
      if (v[i].y > y1) y1 = v[i].y;
   }
   DeferNoteDraw(g, x0, y0, x1 + 1, y1 + 1, textured, tex_mode, clut_x, clut_y, reads_destination);
}

/* ---- the log ---- */

static void DeferPushWords(const uint32_t *words, uint32_t count)
{
   if (Defer.npool + count > Defer.pool_cap)
   {
      while (Defer.npool + count > Defer.pool_cap)
         Defer.pool_cap = Defer.pool_cap ? Defer.pool_cap * 2 : 65536;
      Defer.pool = (uint32_t*)realloc(Defer.pool, Defer.pool_cap * sizeof(uint32_t));
   }
   memcpy(Defer.pool + Defer.npool, words, count * sizeof(uint32_t));
   Defer.npool += count;
}

static DeferEntry *DeferNewEntry(uint8_t kind)
{
   DeferEntry *e;
   if (Defer.nentries == Defer.entries_cap)
   {
      Defer.entries_cap = Defer.entries_cap ? Defer.entries_cap * 2 : 4096;
      Defer.entries = (DeferEntry*)realloc(Defer.entries, Defer.entries_cap * sizeof(DeferEntry));
   }
   e = &Defer.entries[Defer.nentries++];
   memset(e, 0, sizeof *e);
   e->kind = kind;
   e->display_mode = GPU.DisplayMode;
   e->display_ystart = GPU.DisplayFB_YStart;
   e->field_ram_readout = GPU.field_ram_readout;
   e->words = Defer.npool;
   return e;
}

static void FBWriteWord(PS_GPU *g, uint32_t InData);
static void DeferReplay(void);
static void DeferReplayPrefix(uint32_t count);

static void DeferLimit(void)
{
   if (Defer.nentries >= DEFER_ENTRY_LIMIT || Defer.npool >= DEFER_POOL_LIMIT)
      DeferReplay();
}

/* Earlier draws wholly inside a fill's rect whose pixels no live command in between reads are dead.
 * Stops at the last fill of the same rect: everything before it was judged against that rect already,
 * with fewer reads in between. Live reads are kept as tiles, so any number of them fits. */
static void DeferKill(uint32_t fill_index)
{
   uint32_t read_tiles[32] = {0};
   const DeferRect rect = Defer.entries[fill_index].write;
   uint32_t i = fill_index;
   while (i-- > 0)
   {
      DeferEntry *e = &Defer.entries[i];
      unsigned r;
      if (e->killer && memcmp(&e->write, &rect, sizeof rect) == 0)
         return;
      if (e->dead)
         continue;
      if (e->drawing && e->has_write && !DeferWraps(e->write) && DeferContains(rect, e->write)
            && !DeferTileMap(read_tiles, e->write, false))
      {
         e->dead = 1;
         continue;
      }
      if (e->nreads == DEFER_READS_ALL)
         return;
      for (r = 0; r < e->nreads; r++)
         DeferTileMap(read_tiles, e->reads[r], true);
   }
}

static void DeferBeginCommand(void)
{
   Defer.note_has_write = false;
   Defer.note_overflow = false;
   Defer.note_nreads = 0;
}

static void DeferEndCommand(uint32_t cc, bool read_fifo, const uint32_t *cb, unsigned len)
{
   DeferEntry *e;
   DeferLimit();
   e = DeferNewEntry(DEFER_COMMAND);
   e->cc = (uint8_t)cc;
   e->read_fifo = read_fifo;
   e->count = len;
   DeferPushWords(cb, len);
   e->drawing = (cc >= 0x20 && cc <= 0x7F) || cc == 0x02;
   e->has_write = Defer.note_has_write;
   e->write = Defer.note_write;
   if (Defer.note_overflow)
      e->nreads = DEFER_READS_ALL;
   else
   {
      e->nreads = Defer.note_nreads;
      memcpy(e->reads, Defer.note_reads, sizeof e->reads);
   }
   if (e->has_write)
   {
      DeferTiles(e->write, true);
      Defer.any_dirty = true;
   }
   Defer.upload_open = false;
   /* A fill writes every row of its rect unless LineSkipTest skips some (480i without dfe). */
   if (cc == 0x02 && e->has_write && !DeferWraps(e->write) && (e->display_mode & 0x24) != 0x24)
   {
      uint32_t previous = Defer.nentries - 1;
      e->killer = 1;
      DeferKill(Defer.nentries - 1);
      /* Everything before the last fill has met every fill that can kill it now (a double buffer's
       * draws die at the fill after next): draw it, so the log stays a frame or two long. */
      while (previous-- > 0)
         if (Defer.entries[previous].killer)
         {
            DeferReplayPrefix(previous);
            break;
         }
   }
}

static void DeferUploadWord(uint32_t word)
{
   DeferEntry *e;
   if (!Defer.upload_open || Defer.nentries == 0 || Defer.entries[Defer.nentries - 1].kind != DEFER_UPLOAD)
   {
      DeferLimit();
      e = DeferNewEntry(DEFER_UPLOAD);
      Defer.upload_open = true;
      /* A settle may have replayed the FBWrite this continues: the rest of its rect is pending again. */
      e->has_write = 1;
      e->write = DeferMakeRect((int32_t)GPU.FBRW_X, (int32_t)GPU.FBRW_Y, (int32_t)GPU.FBRW_W, (int32_t)GPU.FBRW_H);
      DeferTiles(e->write, true);
      Defer.any_dirty = true;
   }
   else
      e = &Defer.entries[Defer.nentries - 1];
   DeferPushWords(&word, 1);
   e->count++;
}

static void DeferUploadDone(void)
{
   Defer.upload_open = false;
}

static void DeferPolylineEnd(void)
{
   DeferLimit();
   DeferNewEntry(DEFER_PLINE_END);
   Defer.upload_open = false;
}

/* The pixel GPU takes the logging GPU's registers; its texture cache, which only drawing fills,
 * stays its own. */
static void DeferSyncPixels(void)
{
   PS_GPU *p = Defer.pixels;
   struct TexCache_t cache[256];
   memcpy(cache, p->TexCache, sizeof cache);
   *p = GPU;
   memcpy(p->TexCache, cache, sizeof cache);
   p->defer_log = false;
   p->no_pixels = false;
}

static void DeferClear(void)
{
   Defer.nentries = 0;
   Defer.npool = 0;
   Defer.upload_open = false;
   memset(Defer.dirty, 0, sizeof Defer.dirty);
   Defer.any_dirty = false;
}

void GPU_SettlePixels(int32_t x, int32_t y, int32_t w, int32_t h)
{
   if (Defer.on && Defer.any_dirty && DeferTiles(DeferMakeRect(x, y, w, h), false))
      DeferReplay();
}

void GPU_SettleAllPixels(void)
{
   if (Defer.on && Defer.nentries)
      DeferReplay();
}

void GPU_SetDeferPixels(bool on)
{
   if (on == Defer.on)
      return;
   if (on)
   {
      Defer.pixels = (PS_GPU*)calloc(1, sizeof(PS_GPU));
      *Defer.pixels = GPU;
      Defer.pixels->defer_log = false;
      Defer.pixels->no_pixels = false;
      DeferClear();
      Defer.on = true;
      GPU.defer_log = true;
      GPU.no_pixels = true;
      return;
   }
   GPU_SettleAllPixels();
   memcpy(GPU.TexCache, Defer.pixels->TexCache, sizeof GPU.TexCache);
   Defer.on = false;
   GPU.defer_log = false;
   GPU.no_pixels = false;
   free(Defer.pixels);
   free(Defer.entries);
   free(Defer.pool);
   Defer.pixels = NULL;
   Defer.entries = NULL;
   Defer.pool = NULL;
   Defer.entries_cap = Defer.pool_cap = 0;
   DeferClear();
}
