/*
 *  MPEGTS table support
 *  Copyright (C) 2013 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "input.h"

#include <assert.h>

void
mpegts_table_consistency_check ( mpegts_mux_t *mm )
{
#if ENABLE_TRACE
  int i, c = 0;
  mpegts_table_t *mt;

  lock_assert(&mm->mm_tables_lock);

  i = mm->mm_num_tables;
  LIST_FOREACH(mt, &mm->mm_tables, mt_link)
    c++;

  if (i != c) {
    tvherror("mpegts", "table: mux %p count inconsistency (num %d, list %d)", mm, i, c);
    abort();
  }
#endif
}

static void
mpegts_table_fastswitch ( mpegts_mux_t *mm )
{
  char buf[256];
  mpegts_table_t   *mt;

  if(mm->mm_scan_state != MM_SCAN_STATE_ACTIVE)
    return;

  pthread_mutex_lock(&mm->mm_tables_lock);
  LIST_FOREACH(mt, &mm->mm_tables, mt_link) {
    if (!(mt->mt_flags & MT_QUICKREQ) && !mt->mt_working)
      continue;
    if(!mt->mt_complete || mt->mt_working) {
      pthread_mutex_unlock(&mm->mm_tables_lock);
      return;
    }
  }
  pthread_mutex_unlock(&mm->mm_tables_lock);

  mpegts_mux_nice_name(mm, buf, sizeof(buf));
  tvhinfo("mpegts", "%s scan complete", buf);
  mpegts_mux_scan_done(mm, buf, 1);
}

void
mpegts_table_dispatch
  ( const uint8_t *sec, size_t r, void *aux )
{
  int tid, len, ret;
  mpegts_table_t *mt = aux;
  int chkcrc = mt->mt_flags & MT_CRC;

  if(mt->mt_destroyed)
    return;

  /* Table info */
  tid = sec[0];
  len = ((sec[1] & 0x0f) << 8) | sec[2];

  if (tid == 0x72) { /* stuffing section */
    if (len != r - 3) {
      if (tvhlog_limit(&mt->mt_err_log, 10))
        tvhwarn(mt->mt_name, "stuffing found with trailing data "
                             "(len %i, total %zi, errors %zi)",
                             len, r, mt->mt_err_log.count);
    }
    dvb_table_reset((mpegts_psi_table_t *)mt);
    return;
  }

  /* It seems some hardware (or is it the dvb API?) does not
     honour the DMX_CHECK_CRC flag, so we check it again */
  if(chkcrc && tvh_crc32(sec, r, 0xffffffff)) {
    if (tvhlog_limit(&mt->mt_err_log, 10))
      tvhwarn(mt->mt_name, "invalid checksum (len %zi, errors %zi)",
                           r, mt->mt_err_log.count);
    return;
  }

  /* Not enough data */
  if(len < r - 3) {
    tvhtrace(mt->mt_name, "not enough data, %d < %d", (int)r, len);
    return;
  }

  /* Check table mask */
  if((tid & mt->mt_mask) != mt->mt_table)
    return;

  /* Strip trailing CRC */
  if(chkcrc)
    len -= 4;

  /* Pass with tableid / len in data */
  if (mt->mt_flags & MT_FULL)
    ret = mt->mt_callback(mt, sec, len+3, tid);

  /* Pass w/out tableid/len in data */
  else
    ret = mt->mt_callback(mt, sec+3, len, tid);
  
  /* Good */
  if(ret >= 0)
    mt->mt_count++;

  if(!ret && mt->mt_flags & (MT_QUICKREQ|MT_FASTSWITCH))
    mpegts_table_fastswitch(mt->mt_mux);
}

void
mpegts_table_release_ ( mpegts_table_t *mt )
{
  dvb_table_release((mpegts_psi_table_t *)mt);
  tvhtrace("mpegts", "table: mux %p free %s %02X/%02X (%d) pid %04X (%d)",
           mt->mt_mux, mt->mt_name, mt->mt_table, mt->mt_mask, mt->mt_table,
           mt->mt_pid, mt->mt_pid);
  if (mt->mt_bat)
    dvb_bat_destroy(mt);
  if (mt->mt_destroy)
    mt->mt_destroy(mt);
  free(mt->mt_name);
#if ENABLE_TRACE
  /* poison */
  memset(mt, 0xa5, sizeof(*mt));
#endif
  free(mt);
}

static void
mpegts_table_destroy_ ( mpegts_table_t *mt )
{
  mpegts_mux_t *mm = mt->mt_mux;

  lock_assert(&mm->mm_tables_lock);

  tvhtrace("mpegts", "table: mux %p destroy %s %02X/%02X (%d) pid %04X (%d)",
           mm, mt->mt_name, mt->mt_table, mt->mt_mask, mt->mt_table,
           mt->mt_pid, mt->mt_pid);
  mpegts_table_consistency_check(mm);
  mt->mt_destroyed = 1;
  mt->mt_mux->mm_close_table(mt->mt_mux, mt);
  mpegts_table_consistency_check(mm);
  mpegts_table_release(mt);
}

void
mpegts_table_destroy ( mpegts_table_t *mt )
{
  mpegts_mux_t *mm = mt->mt_mux;

  pthread_mutex_lock(&mm->mm_tables_lock);
  mpegts_table_destroy_(mt);
  pthread_mutex_unlock(&mm->mm_tables_lock);
}

/**
 * Determine table type
 */
int
mpegts_table_type ( mpegts_table_t *mt )
{
  int type = 0;
  if (mt->mt_flags & MT_FAST) type |= MPS_FTABLE;
  if (mt->mt_flags & MT_SLOW) type |= MPS_TABLE;
  if (mt->mt_flags & MT_RECORD) type |= MPS_STREAM;
  if ((type & (MPS_FTABLE | MPS_TABLE)) == 0) type |= MPS_TABLE;
  return type;
}

/**
 * Add a new DVB table
 */
mpegts_table_t *
mpegts_table_add
  ( mpegts_mux_t *mm, int tableid, int mask,
    mpegts_table_callback_t callback, void *opaque,
    const char *name, int flags, int pid )
{
  mpegts_table_t *mt;
  int subscribe = 1;

  /* Check for existing */
  pthread_mutex_lock(&mm->mm_tables_lock);
  mpegts_table_consistency_check(mm);
  LIST_FOREACH(mt, &mm->mm_tables, mt_link) {
    if (mt->mt_opaque != opaque)
      continue;
    if (mt->mt_pid < 0) {
      if (strcmp(mt->mt_name, name))
        continue;
      mt->mt_callback   = callback;
      mt->mt_pid        = pid;
      mt->mt_table      = tableid;
      mm->mm_open_table(mm, mt, 1);
    } else if (pid >= 0) {
      if (mt->mt_pid != pid)
        continue;
      if (mt->mt_callback != callback)
        continue;
    } else {
      if (strcmp(mt->mt_name, name))
        continue;
      if (!(flags & MT_SKIPSUBS) && !mt->mt_subscribed)
        mm->mm_open_table(mm, mt, 1);
    }
    mpegts_table_consistency_check(mm);
    pthread_mutex_unlock(&mm->mm_tables_lock);
    return mt;
  }
  tvhtrace("mpegts", "table: mux %p add %s %02X/%02X (%d) pid %04X (%d)",
           mm, name, tableid, mask, tableid, pid, pid);

  /* Create */
  mt = calloc(1, sizeof(mpegts_table_t));
  mt->mt_arefcount = 1;
  mt->mt_name      = strdup(name);
  mt->mt_callback  = callback;
  mt->mt_opaque    = opaque;
  mt->mt_pid       = pid;
  mt->mt_flags     = flags & ~(MT_SKIPSUBS|MT_SCANSUBS);
  mt->mt_table     = tableid;
  mt->mt_mask      = mask;
  mt->mt_mux       = mm;
  mt->mt_cc        = -1;

  /* Open table */
  if (pid < 0) {
    subscribe = 0;
  } else if (flags & MT_SKIPSUBS) {
    subscribe = 0;
  } else if (flags & MT_SCANSUBS) {
    if (mm->mm_scan_state == MM_SCAN_STATE_IDLE)
      subscribe = 0;
  }
  mm->mm_open_table(mm, mt, subscribe);
  mpegts_table_consistency_check(mm);
  pthread_mutex_unlock(&mm->mm_tables_lock);
  return mt;
}

/**
 *
 */
void
mpegts_table_flush_all ( mpegts_mux_t *mm )
{
  mpegts_table_t        *mt;

  descrambler_flush_tables(mm);
  pthread_mutex_lock(&mm->mm_tables_lock);
  mpegts_table_consistency_check(mm);
  while ((mt = TAILQ_FIRST(&mm->mm_defer_tables))) {
    TAILQ_REMOVE(&mm->mm_defer_tables, mt, mt_defer_link);
    mt->mt_defer_cmd = 0;
    mpegts_table_release(mt);
  }
  while ((mt = LIST_FIRST(&mm->mm_tables))) {
    mt->mt_flags &= ~MT_DEFER; /* force destroy */
    mpegts_table_consistency_check(mm);
    mpegts_table_destroy_(mt);
    mpegts_table_consistency_check(mm);
  }
  assert(mm->mm_num_tables == 0);
  assert(TAILQ_FIRST(&mm->mm_defer_tables) == NULL);
  assert(LIST_FIRST(&mm->mm_tables) == NULL);
  pthread_mutex_unlock(&mm->mm_tables_lock);
}


/******************************************************************************
 * Editor Configuration
 *
 * vim:sts=2:ts=2:sw=2:et
 *****************************************************************************/
