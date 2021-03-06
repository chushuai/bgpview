/*
 * Copyright (C) 2014 The Regents of the University of California.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "bgpcorsaro_int.h"
#include "config.h"

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif

#include "bgpcorsaro.h"
#include "bgpcorsaro_io.h"
#include "bgpcorsaro_log.h"
#include "bgpcorsaro_plugin.h"

#include "utils.h"

static char *stradd(const char *str, char *bufp, char *buflim)
{
  while (bufp < buflim && (*bufp = *str++) != '\0') {
    ++bufp;
  }
  return bufp;
}

static char *generate_file_name(bgpcorsaro_t *bc, const char *plugin,
                                bgpcorsaro_interval_t *interval,
                                int compress_type)
{
  /* some of the structure of this code is borrowed from the
     FreeBSD implementation of strftime */

  /* the output buffer */
  /* @todo change the code to dynamically realloc this if we need more
     space */
  char buf[1024];
  char tbuf[1024];
  char *bufp = buf;
  char *buflim = buf + sizeof(buf);

  const char *tmpl, *end = NULL;
  char secs[11]; /* length of UINT32_MAX +1 */
  time_t t;

  if (bc->compress != WANDIO_COMPRESS_NONE && bc->compress != compress_type) {
    // suffix on template doesn't correspond to desired compress_type
    end = strrchr(bc->template, '.');
  }
  if (!end) {
    end = strchr(bc->template, '\0');
  }

  for (tmpl = bc->template; tmpl != end; ++tmpl) {
    if (*tmpl == '%') {
      switch (*++tmpl) {
      case '\0':
        --tmpl;
        break;

      /* BEWARE: if you add a new pattern here, you must also add it to
       * bgpcorsaro_io_template_has_timestamp */

      case BGPCORSARO_IO_MONITOR_PATTERN:
        bufp = stradd(bc->monitorname, bufp, buflim);
        continue;

      case BGPCORSARO_IO_PLUGIN_PATTERN:
        bufp = stradd(plugin, bufp, buflim);
        continue;

      case 's':
        if (interval != NULL) {
          snprintf(secs, sizeof(secs), "%" PRIu32, interval->time);
          bufp = stradd(secs, bufp, buflim);
          continue;
        }
      /* fall through */
      default:
        /* we want to be generous and leave non-recognized formats
           intact - especially for strftime to use */
        --tmpl;
      }
    }
    if (bufp == buflim)
      break;
    *bufp++ = *tmpl;
  }

  *bufp = '\0';

  /* now let strftime have a go */
  if (interval != NULL) {
    t = interval->time;
    strftime(tbuf, sizeof(tbuf), buf, gmtime(&t));
    return strdup(tbuf);
  }

  return strdup(buf);
}

/* == EXPORTED FUNCTIONS BELOW THIS POINT == */

iow_t *bgpcorsaro_io_prepare_file(bgpcorsaro_t *bc,
                                  const char *plugin_name,
                                  bgpcorsaro_interval_t *interval)
{
  return bgpcorsaro_io_prepare_file_full(bc, plugin_name, interval,
                                         bc->compress,
                                         bc->compress_level, O_CREAT);
}

iow_t *bgpcorsaro_io_prepare_file_full(bgpcorsaro_t *bc,
                                       const char *plugin_name,
                                       bgpcorsaro_interval_t *interval,
                                       int compress_type, int compress_level,
                                       int flags)
{
  iow_t *f = NULL;
  char *outfileuri;

  /* generate a file name based on the plugin name */
  if ((outfileuri = generate_file_name(bc, plugin_name, interval,
                                       compress_type)) == NULL) {
    bgpcorsaro_log(__func__, bc, "could not generate file name for %s",
                   plugin_name);
    return NULL;
  }

  if ((f = wandio_wcreate(outfileuri, compress_type, compress_level, flags)) ==
      NULL) {
    bgpcorsaro_log(__func__, bc, "could not open %s for writing", outfileuri);
    return NULL;
  }

  free(outfileuri);
  return f;
}

int bgpcorsaro_io_validate_template(bgpcorsaro_t *bc, char *template)
{
  /* be careful using bgpcorsaro here, it is likely not initialized fully */

  /* check for length first */
  if (template == NULL) {
    bgpcorsaro_log(__func__, bc, "output template must be set");
    return 0;
  }

  /* check that the plugin pattern is in the template */
  if (strstr(template, BGPCORSARO_IO_PLUGIN_PATTERN_STR) == NULL) {
    bgpcorsaro_log(__func__, bc, "template string must contain %s",
                   BGPCORSARO_IO_PLUGIN_PATTERN_STR);
    return 0;
  }

  /* we're good! */
  return 1;
}

int bgpcorsaro_io_template_has_timestamp(bgpcorsaro_t *bc)
{
  assert(bc->template);
  /* be careful using bgpcorsaro here, this is called pre-start */

  /* the easiest (but not easiest to maintain) way to do this is to step through
   * each '%' character in the string and check what is after it. if it is
   * anything other than P (for plugin) or N (for monitor name), then it is a
   * timestamp. HOWEVER. If new bgpcorsaro-specific patterns are added, they
   * must also be added here. gross */

  for (char *p = bc->template; *p; ++p) {
    if (*p == '%') {
      /* BEWARE: if you add a new pattern here, you must also add it to
       * generate_file_name */
      if (*(p + 1) != BGPCORSARO_IO_MONITOR_PATTERN &&
          *(p + 1) != BGPCORSARO_IO_PLUGIN_PATTERN) {
        return 1;
      }
    }
  }
  return 0;
}

off_t bgpcorsaro_io_write_interval_start(bgpcorsaro_t *bc, iow_t *file,
                                         bgpcorsaro_interval_t *int_start)
{
  return wandio_printf(file, "# BGPCORSARO_INTERVAL_START %d %ld\n",
                       int_start->number, int_start->time);
}

off_t bgpcorsaro_io_write_interval_end(bgpcorsaro_t *bc, iow_t *file,
                                       bgpcorsaro_interval_t *int_end)
{
  return wandio_printf(file, "# BGPCORSARO_INTERVAL_END %d %ld\n",
                       int_end->number, int_end->time);
}

off_t bgpcorsaro_io_write_plugin_start(bgpcorsaro_t *bc, iow_t *file,
                                       bgpcorsaro_plugin_t *plugin)
{
  assert(plugin != NULL);

  return wandio_printf(file, "# BGPCORSARO_PLUGIN_DATA_START %s\n",
                       PLUGIN_NAME);
}

off_t bgpcorsaro_io_write_plugin_end(bgpcorsaro_t *bc, iow_t *file,
                                     bgpcorsaro_plugin_t *plugin)
{
  assert(plugin != NULL);

  return wandio_printf(file, "# BGPCORSARO_PLUGIN_DATA_END %s\n", PLUGIN_NAME);
}
