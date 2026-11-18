/* gpu_defer_replay.c -- gpu_defer.c's replay: the pixel GPU runs the log in order. #included by gpu.c
 * after the command table. */

/* One upload word into VRAM at the FBWrite cursor; a GPU without pixels only moves the cursor. */
static void FBWriteWord(PS_GPU *g, uint32_t InData)
{
   const bool pixels = !g->no_pixels;
   const bool sw = rhi_intf_has_software_renderer() && pixels;
   unsigned i;
   for(i = 0; i < 2; i++)
   {
      bool fetch = false;
      if (sw)
         fetch = texel_fetch(g, g->FBRW_CurX & 1023, g->FBRW_CurY & 511) & g->MaskEvalAND;

      if (!fetch && pixels)
         texel_put(g->FBRW_CurX & 1023, g->FBRW_CurY & 511, InData | g->MaskSetOR);

      g->FBRW_CurX++;
      if(g->FBRW_CurX == (g->FBRW_X + g->FBRW_W))
      {
         g->FBRW_CurX = g->FBRW_X;
         g->FBRW_CurY++;
         if(g->FBRW_CurY == (g->FBRW_Y + g->FBRW_H))
         {
            if (pixels)
               rhi_intf_load_image(
                     g->FBRW_X, g->FBRW_Y,
                     g->FBRW_W, g->FBRW_H,
                     g->vram,
                     g->MaskEvalAND != 0,
                     g->MaskSetOR != 0);
            g->InCmd = INCMD_NONE;
            break;   /* Break out of the for() loop. */
         }
      }
      InData >>= 16;
   }
}

/* The pixel GPU runs the first `count` entries; the rest stay pending. */
static void DeferReplayPrefix(uint32_t count)
{
   PS_GPU *p = Defer.pixels;
   uint32_t i;
   if (!Defer.on || Defer.settling || count == 0)
      return;
   Defer.settling = true;
   for (i = 0; i < count; i++)
   {
      const DeferEntry *e = &Defer.entries[i];
      const uint32_t *cb = Defer.pool + e->words;
      p->DisplayMode = e->display_mode;
      p->DisplayFB_YStart = e->display_ystart;
      p->field_ram_readout = e->field_ram_readout;
      p->no_pixels = e->dead;
      p->DrawTimeAvail = 0x20000000;
      switch (e->kind)
      {
         case DEFER_UPLOAD:
         {
            uint32_t w;
            for (w = 0; w < e->count && p->InCmd == INCMD_FBWRITE; w++)
               FBWriteWord(p, cb[w]);
            break;
         }
         case DEFER_PLINE_END:
            p->InCmd = INCMD_NONE;
            break;
         default:
         {
            const uint32_t cc = e->cc;
            if (!e->read_fifo && cc >= 0x20 && cc <= 0x3F && (cc & 0x4))
               SetTPage(p, cb[4 + ((cc >> 4) & 0x1)] >> 16);
            if (cc >= 0x80 && cc <= 0x9F)
               Command_FBCopy(p, cb);
            else if (cc >= 0xA0 && cc <= 0xBF)
               Command_FBWrite(p, cb);
            else if (cc >= 0xC0 && cc <= 0xDF)
               InvalidateTexCache(p); /* the read itself was served from settled VRAM */
            else
            {
               const CTEntry *command = &Commands[cc];
               if (command->func[p->abr][p->TexMode])
                  command->func[p->abr][p->TexMode | (p->MaskEvalAND ? 0x4 : 0x0)](p, cb);
            }
            break;
         }
      }
   }
   p->no_pixels = false;
   if (count >= Defer.nentries)
      DeferClear();
   else
   {
      const uint32_t base = Defer.entries[count].words;
      memmove(Defer.pool, Defer.pool + base, (Defer.npool - base) * sizeof(uint32_t));
      Defer.npool -= base;
      memmove(Defer.entries, Defer.entries + count, (Defer.nentries - count) * sizeof(DeferEntry));
      Defer.nentries -= count;
      memset(Defer.dirty, 0, sizeof Defer.dirty);
      Defer.any_dirty = false;
      for (i = 0; i < Defer.nentries; i++)
      {
         Defer.entries[i].words -= base;
         if (Defer.entries[i].has_write)
         {
            DeferTiles(Defer.entries[i].write, true);
            Defer.any_dirty = true;
         }
      }
   }
   Defer.settling = false;
}

static void DeferReplay(void)
{
   DeferReplayPrefix(Defer.nentries);
}

