/**
 * @file
 * Routines for managing attachments
 *
 * @authors
 * Copyright (C) 1996-2000,2002,2007,2010 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1999-2006 Thomas Roessler <roessler@does-not-exist.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include <libintl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mutt.h"
#include "attach.h"
#include "body.h"
#include "context.h"
#include "envelope.h"
#include "filter.h"
#include "format_flags.h"
#include "globals.h"
#include "header.h"
#include "keymap.h"
#include "keymap_defs.h"
#include "lib/lib.h"
#include "mailbox.h"
#include "mapping.h"
#include "mime.h"
#include "mutt_curses.h"
#include "mutt_menu.h"
#include "mx.h"
#include "ncrypt/ncrypt.h"
#include "options.h"
#include "protos.h"
#include "rfc1524.h"
#include "state.h"

static const char *Mailbox_is_read_only = N_("Mailbox is read-only.");

#define CHECK_READONLY                                                         \
  if (Context->readonly)                                                       \
  {                                                                            \
    mutt_flushinp();                                                           \
    mutt_error(_(Mailbox_is_read_only));                                       \
    break;                                                                     \
  }

static const struct Mapping AttachHelp[] = {
  { N_("Exit"), OP_EXIT },   { N_("Save"), OP_SAVE }, { N_("Pipe"), OP_PIPE },
  { N_("Print"), OP_PRINT }, { N_("Help"), OP_HELP }, { NULL, 0 },
};

void mutt_update_tree(struct AttachPtr **idx, short idxlen)
{
  char buf[STRING];
  char *s = NULL;
  for (int x = 0; x < idxlen; x++)
  {
    idx[x]->num = x;
    if (2 * (idx[x]->level + 2) < sizeof(buf))
    {
      if (idx[x]->level)
      {
        s = buf + 2 * (idx[x]->level - 1);
        *s++ = (idx[x]->content->next) ? MUTT_TREE_LTEE : MUTT_TREE_LLCORNER;
        *s++ = MUTT_TREE_HLINE;
        *s++ = MUTT_TREE_RARROW;
      }
      else
        s = buf;
      *s = 0;
    }

    if (idx[x]->tree)
    {
      if (mutt_strcmp(idx[x]->tree, buf) != 0)
        mutt_str_replace(&idx[x]->tree, buf);
    }
    else
      idx[x]->tree = safe_strdup(buf);

    if (2 * (idx[x]->level + 2) < sizeof(buf) && idx[x]->level)
    {
      s = buf + 2 * (idx[x]->level - 1);
      *s++ = (idx[x]->content->next) ? '\005' : '\006';
      *s++ = '\006';
    }
  }
}

struct AttachPtr **mutt_gen_attach_list(struct Body *m, int parent_type,
                                        struct AttachPtr **idx, short *idxlen,
                                        short *idxmax, int level, int compose)
{
  struct AttachPtr *new = NULL;

  for (; m; m = m->next)
  {
    if (*idxlen == *idxmax)
    {
      safe_realloc(&idx, sizeof(struct AttachPtr *) * ((*idxmax) += 5));
      for (int i = *idxlen; i < *idxmax; i++)
        idx[i] = NULL;
    }

    if (m->type == TYPEMULTIPART && m->parts &&
        (compose || (parent_type == -1 && (mutt_strcasecmp("alternative", m->subtype) != 0))) &&
        (!(WithCrypto & APPLICATION_PGP) || !mutt_is_multipart_encrypted(m)))
    {
      idx = mutt_gen_attach_list(m->parts, m->type, idx, idxlen, idxmax, level, compose);
    }
    else
    {
      if (!idx[*idxlen])
        idx[*idxlen] = safe_calloc(1, sizeof(struct AttachPtr));

      new = idx[(*idxlen)++];
      new->content = m;
      m->aptr = new;
      new->parent_type = parent_type;
      new->level = level;

      /* We don't support multipart messages in the compose menu yet */
      if (!compose && !m->collapsed &&
          ((m->type == TYPEMULTIPART &&
            (!(WithCrypto & APPLICATION_PGP) || !mutt_is_multipart_encrypted(m))) ||
           mutt_is_message_type(m->type, m->subtype)))
      {
        idx = mutt_gen_attach_list(m->parts, m->type, idx, idxlen, idxmax, level + 1, compose);
      }
    }
  }

  if (level == 0)
    mutt_update_tree(idx, *idxlen);

  return idx;
}

/**
 * mutt_attach_fmt - Format string for attachment menu
 *
 * | Expando | Description
 * |:--------|:--------------------------------------------------------
 * | \%c     | character set: convert?
 * | \%C     | character set
 * | \%D     | deleted flag
 * | \%d     | description
 * | \%e     | MIME content-transfer-encoding
 * | \%F     | filename for content-disposition header
 * | \%f     | filename
 * | \%I     | content-disposition, either I (inline) or A (attachment)
 * | \%t     | tagged flag
 * | \%T     | tree chars
 * | \%m     | major MIME type
 * | \%M     | MIME subtype
 * | \%n     | attachment number
 * | \%s     | size
 * | \%u     | unlink
 */
const char *mutt_attach_fmt(char *dest, size_t destlen, size_t col, int cols,
                            char op, const char *src, const char *prefix,
                            const char *ifstring, const char *elsestring,
                            unsigned long data, enum FormatFlag flags)
{
  char fmt[16];
  char tmp[SHORT_STRING];
  char charset[SHORT_STRING];
  struct AttachPtr *aptr = (struct AttachPtr *) data;
  int optional = (flags & MUTT_FORMAT_OPTIONAL);
  size_t l;

  switch (op)
  {
    case 'C':
      if (!optional)
      {
        if (mutt_is_text_part(aptr->content) &&
            mutt_get_body_charset(charset, sizeof(charset), aptr->content))
          mutt_format_s(dest, destlen, prefix, charset);
        else
          mutt_format_s(dest, destlen, prefix, "");
      }
      else if (!mutt_is_text_part(aptr->content) ||
               !mutt_get_body_charset(charset, sizeof(charset), aptr->content))
        optional = 0;
      break;
    case 'c':
      /* XXX */
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sc", prefix);
        snprintf(dest, destlen, fmt,
                 aptr->content->type != TYPETEXT || aptr->content->noconv ? 'n' : 'c');
      }
      else if (aptr->content->type != TYPETEXT || aptr->content->noconv)
        optional = 0;
      break;
    case 'd':
      if (!optional)
      {
        if (aptr->content->description)
        {
          mutt_format_s(dest, destlen, prefix, aptr->content->description);
          break;
        }
        if (mutt_is_message_type(aptr->content->type, aptr->content->subtype) &&
            MsgFmt && aptr->content->hdr)
        {
          char s[SHORT_STRING];
          _mutt_make_string(s, sizeof(s), MsgFmt, NULL, aptr->content->hdr,
                            MUTT_FORMAT_FORCESUBJ | MUTT_FORMAT_MAKEPRINT | MUTT_FORMAT_ARROWCURSOR);
          if (*s)
          {
            mutt_format_s(dest, destlen, prefix, s);
            break;
          }
        }
        if (!aptr->content->d_filename && !aptr->content->filename)
        {
          mutt_format_s(dest, destlen, prefix, "<no description>");
          break;
        }
      }
      else if (aptr->content->description ||
               (mutt_is_message_type(aptr->content->type, aptr->content->subtype) &&
                MsgFmt && aptr->content->hdr))
        break;
    /* FALLS THROUGH TO 'F' */
    case 'F':
      if (!optional)
      {
        if (aptr->content->d_filename)
        {
          mutt_format_s(dest, destlen, prefix, aptr->content->d_filename);
          break;
        }
      }
      else if (!aptr->content->d_filename && !aptr->content->filename)
      {
        optional = 0;
        break;
      }
    /* FALLS THROUGH TO 'f' */
    case 'f':
      if (!optional)
      {
        if (aptr->content->filename && *aptr->content->filename == '/')
        {
          char path[_POSIX_PATH_MAX];

          strfcpy(path, aptr->content->filename, sizeof(path));
          mutt_pretty_mailbox(path, sizeof(path));
          mutt_format_s(dest, destlen, prefix, path);
        }
        else
          mutt_format_s(dest, destlen, prefix, NONULL(aptr->content->filename));
      }
      else if (!aptr->content->filename)
        optional = 0;
      break;
    case 'D':
      if (!optional)
        snprintf(dest, destlen, "%c", aptr->content->deleted ? 'D' : ' ');
      else if (!aptr->content->deleted)
        optional = 0;
      break;
    case 'e':
      if (!optional)
        mutt_format_s(dest, destlen, prefix, ENCODING(aptr->content->encoding));
      break;
    case 'I':
      if (!optional)
      {
        const char dispchar[] = { 'I', 'A', 'F', '-' };
        char ch;

        if (aptr->content->disposition < sizeof(dispchar))
          ch = dispchar[aptr->content->disposition];
        else
        {
          mutt_debug(1, "ERROR: invalid content-disposition %d\n", aptr->content->disposition);
          ch = '!';
        }
        snprintf(dest, destlen, "%c", ch);
      }
      break;
    case 'm':
      if (!optional)
        mutt_format_s(dest, destlen, prefix, TYPE(aptr->content));
      break;
    case 'M':
      if (!optional)
        mutt_format_s(dest, destlen, prefix, aptr->content->subtype);
      else if (!aptr->content->subtype)
        optional = 0;
      break;
    case 'n':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prefix);
        snprintf(dest, destlen, fmt, aptr->num + 1);
      }
      break;
    case 'Q':
      if (optional)
        optional = aptr->content->attach_qualifies;
      else
      {
        snprintf(fmt, sizeof(fmt), "%%%sc", prefix);
        mutt_format_s(dest, destlen, fmt, "Q");
      }
      break;
    case 's':
      if (flags & MUTT_FORMAT_STAT_FILE)
      {
        struct stat st;
        stat(aptr->content->filename, &st);
        l = st.st_size;
      }
      else
        l = aptr->content->length;

      if (!optional)
      {
        mutt_pretty_size(tmp, sizeof(tmp), l);
        mutt_format_s(dest, destlen, prefix, tmp);
      }
      else if (l == 0)
        optional = 0;

      break;
    case 't':
      if (!optional)
        snprintf(dest, destlen, "%c", aptr->content->tagged ? '*' : ' ');
      else if (!aptr->content->tagged)
        optional = 0;
      break;
    case 'T':
      if (!optional)
        mutt_format_s_tree(dest, destlen, prefix, NONULL(aptr->tree));
      else if (!aptr->tree)
        optional = 0;
      break;
    case 'u':
      if (!optional)
        snprintf(dest, destlen, "%c", aptr->content->unlink ? '-' : ' ');
      else if (!aptr->content->unlink)
        optional = 0;
      break;
    case 'X':
      if (optional)
        optional = (aptr->content->attach_count + aptr->content->attach_qualifies) != 0;
      else
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prefix);
        snprintf(dest, destlen, fmt,
                 aptr->content->attach_count + aptr->content->attach_qualifies);
      }
      break;
    default:
      *dest = 0;
  }

  if (optional)
    mutt_expando_format(dest, destlen, col, cols, ifstring, mutt_attach_fmt, data, 0);
  else if (flags & MUTT_FORMAT_OPTIONAL)
    mutt_expando_format(dest, destlen, col, cols, elsestring, mutt_attach_fmt, data, 0);
  return src;
}

static void attach_entry(char *b, size_t blen, struct Menu *menu, int num)
{
  mutt_expando_format(b, blen, 0, MuttIndexWindow->cols, NONULL(AttachFormat), mutt_attach_fmt,
                    (unsigned long) (((struct AttachPtr **) menu->data)[num]),
                    MUTT_FORMAT_ARROWCURSOR);
}

int mutt_tag_attach(struct Menu *menu, int n, int m)
{
  struct Body *cur = ((struct AttachPtr **) menu->data)[n]->content;
  bool ot = cur->tagged;

  cur->tagged = (m >= 0 ? m : !cur->tagged);
  return cur->tagged - ot;
}

/**
 * mutt_is_message_type - Determine if a mime type matches a message or not
 * @param type    Message type enum value
 * @param subtype Message subtype
 * @retval true  Type is message/news or message/rfc822
 * @retval false Otherwise
 */
bool mutt_is_message_type(int type, const char *subtype)
{
  if (type != TYPEMESSAGE)
    return false;

  subtype = NONULL(subtype);
  return ((mutt_strcasecmp(subtype, "rfc822") == 0) ||
          (mutt_strcasecmp(subtype, "news") == 0));
}

static void prepend_curdir(char *dst, size_t dstlen)
{
  size_t l;

  if (!dst || !*dst || *dst == '/' || dstlen < 3 ||
      /* XXX bad modularization, these are special to mutt_expand_path() */
      !strchr("~=+@<>!-^", *dst))
    return;

  dstlen -= 3;
  l = strlen(dst) + 2;
  l = (l > dstlen ? dstlen : l);
  memmove(dst + 2, dst, l);
  dst[0] = '.';
  dst[1] = '/';
  dst[l + 2] = 0;
}

static int query_save_attachment(FILE *fp, struct Body *body,
                                 struct Header *hdr, char **directory)
{
  char *prompt = NULL;
  char buf[_POSIX_PATH_MAX], tfile[_POSIX_PATH_MAX];
  int is_message;
  int append = 0;
  int rc;

  if (body->filename)
  {
    if (directory && *directory)
      mutt_concat_path(buf, *directory, mutt_basename(body->filename), sizeof(buf));
    else
      strfcpy(buf, body->filename, sizeof(buf));
  }
  else if (body->hdr && body->encoding != ENCBASE64 && body->encoding != ENCQUOTEDPRINTABLE &&
           mutt_is_message_type(body->type, body->subtype))
    mutt_default_save(buf, sizeof(buf), body->hdr);
  else
    buf[0] = 0;

  prepend_curdir(buf, sizeof(buf));

  prompt = _("Save to file: ");
  while (prompt)
  {
    if (mutt_get_field(prompt, buf, sizeof(buf), MUTT_FILE | MUTT_CLEAR) != 0 || !buf[0])
    {
      mutt_clear_error();
      return -1;
    }

    prompt = NULL;
    mutt_expand_path(buf, sizeof(buf));

    is_message = (fp && body->hdr && body->encoding != ENCBASE64 &&
                  body->encoding != ENCQUOTEDPRINTABLE &&
                  mutt_is_message_type(body->type, body->subtype));

    if (is_message)
    {
      struct stat st;

      /* check to make sure that this file is really the one the user wants */
      if ((rc = mutt_save_confirm(buf, &st)) == 1)
      {
        prompt = _("Save to file: ");
        continue;
      }
      else if (rc == -1)
        return -1;
      strfcpy(tfile, buf, sizeof(tfile));
    }
    else
    {
      if ((rc = mutt_check_overwrite(body->filename, buf, tfile, sizeof(tfile),
                                     &append, directory)) == -1)
        return -1;
      else if (rc == 1)
      {
        prompt = _("Save to file: ");
        continue;
      }
    }

    mutt_message(_("Saving..."));
    if (mutt_save_attachment(fp, body, tfile, append,
                             (hdr || !is_message) ? hdr : body->hdr) == 0)
    {
      mutt_message(_("Attachment saved."));
      return 0;
    }
    else
    {
      prompt = _("Save to file: ");
      continue;
    }
  }
  return 0;
}

void mutt_save_attachment_list(FILE *fp, int tag, struct Body *top,
                               struct Header *hdr, struct Menu *menu)
{
  char buf[_POSIX_PATH_MAX], tfile[_POSIX_PATH_MAX];
  char *directory = NULL;
  int rc = 1;
  int last = menu ? menu->current : -1;
  FILE *fpout = NULL;

  buf[0] = 0;

  for (; top; top = top->next)
  {
    if (!tag || top->tagged)
    {
      if (!option(OPT_ATTACH_SPLIT))
      {
        if (!buf[0])
        {
          int append = 0;

          strfcpy(buf, mutt_basename(NONULL(top->filename)), sizeof(buf));
          prepend_curdir(buf, sizeof(buf));

          if (mutt_get_field(_("Save to file: "), buf, sizeof(buf), MUTT_FILE | MUTT_CLEAR) != 0 ||
              !buf[0])
            return;
          mutt_expand_path(buf, sizeof(buf));
          if (mutt_check_overwrite(top->filename, buf, tfile, sizeof(tfile), &append, NULL))
            return;
          rc = mutt_save_attachment(fp, top, tfile, append, hdr);
          if (rc == 0 && AttachSep && (fpout = fopen(tfile, "a")) != NULL)
          {
            fprintf(fpout, "%s", AttachSep);
            safe_fclose(&fpout);
          }
        }
        else
        {
          rc = mutt_save_attachment(fp, top, tfile, MUTT_SAVE_APPEND, hdr);
          if (rc == 0 && AttachSep && (fpout = fopen(tfile, "a")) != NULL)
          {
            fprintf(fpout, "%s", AttachSep);
            safe_fclose(&fpout);
          }
        }
      }
      else
      {
        if (tag && menu && top->aptr)
        {
          menu->oldcurrent = menu->current;
          menu->current = top->aptr->num;
          menu_check_recenter(menu);
          menu->redraw |= REDRAW_MOTION;

          menu_redraw(menu);
        }
        if (query_save_attachment(fp, top, hdr, &directory) == -1)
          break;
      }
    }
    else if (top->parts)
      mutt_save_attachment_list(fp, 1, top->parts, hdr, menu);
    if (!tag)
      break;
  }

  FREE(&directory);

  if (tag && menu)
  {
    menu->oldcurrent = menu->current;
    menu->current = last;
    menu_check_recenter(menu);
    menu->redraw |= REDRAW_MOTION;
  }

  if (!option(OPT_ATTACH_SPLIT) && (rc == 0))
    mutt_message(_("Attachment saved."));
}

static void query_pipe_attachment(char *command, FILE *fp, struct Body *body, int filter)
{
  char tfile[_POSIX_PATH_MAX];
  char warning[STRING + _POSIX_PATH_MAX];

  if (filter)
  {
    snprintf(warning, sizeof(warning),
             _("WARNING!  You are about to overwrite %s, continue?"), body->filename);
    if (mutt_yesorno(warning, MUTT_NO) != MUTT_YES)
    {
      mutt_window_clearline(MuttMessageWindow, 0);
      return;
    }
    mutt_mktemp(tfile, sizeof(tfile));
  }
  else
    tfile[0] = 0;

  if (mutt_pipe_attachment(fp, body, command, tfile))
  {
    if (filter)
    {
      mutt_unlink(body->filename);
      mutt_rename_file(tfile, body->filename);
      mutt_update_encoding(body);
      mutt_message(_("Attachment filtered."));
    }
  }
  else
  {
    if (filter && tfile[0])
      mutt_unlink(tfile);
  }
}

static void pipe_attachment(FILE *fp, struct Body *b, struct State *state)
{
  FILE *ifp = NULL;

  if (fp)
  {
    state->fpin = fp;
    mutt_decode_attachment(b, state);
    if (AttachSep)
      state_puts(AttachSep, state);
  }
  else
  {
    if ((ifp = fopen(b->filename, "r")) == NULL)
    {
      mutt_perror("fopen");
      return;
    }
    mutt_copy_stream(ifp, state->fpout);
    safe_fclose(&ifp);
    if (AttachSep)
      state_puts(AttachSep, state);
  }
}

static void pipe_attachment_list(char *command, FILE *fp, int tag,
                                 struct Body *top, int filter, struct State *state)
{
  for (; top; top = top->next)
  {
    if (!tag || top->tagged)
    {
      if (!filter && !option(OPT_ATTACH_SPLIT))
        pipe_attachment(fp, top, state);
      else
        query_pipe_attachment(command, fp, top, filter);
    }
    else if (top->parts)
      pipe_attachment_list(command, fp, tag, top->parts, filter, state);
    if (!tag)
      break;
  }
}

void mutt_pipe_attachment_list(FILE *fp, int tag, struct Body *top, int filter)
{
  struct State state;
  char buf[SHORT_STRING];
  pid_t thepid;

  if (fp)
    filter = 0; /* sanity check: we can't filter in the recv case yet */

  buf[0] = 0;
  memset(&state, 0, sizeof(struct State));
  /* perform charset conversion on text attachments when piping */
  state.flags = MUTT_CHARCONV;

  if (mutt_get_field((filter ? _("Filter through: ") : _("Pipe to: ")), buf,
                     sizeof(buf), MUTT_CMD) != 0 ||
      !buf[0])
    return;

  mutt_expand_path(buf, sizeof(buf));

  if (!filter && !option(OPT_ATTACH_SPLIT))
  {
    mutt_endwin(NULL);
    thepid = mutt_create_filter(buf, &state.fpout, NULL, NULL);
    pipe_attachment_list(buf, fp, tag, top, filter, &state);
    safe_fclose(&state.fpout);
    if (mutt_wait_filter(thepid) != 0 || option(OPT_WAIT_KEY))
      mutt_any_key_to_continue(NULL);
  }
  else
    pipe_attachment_list(buf, fp, tag, top, filter, &state);
}

static int can_print(struct Body *top, int tag)
{
  char type[STRING];

  for (; top; top = top->next)
  {
    snprintf(type, sizeof(type), "%s/%s", TYPE(top), top->subtype);
    if (!tag || top->tagged)
    {
      if (!rfc1524_mailcap_lookup(top, type, NULL, MUTT_PRINT))
      {
        if ((mutt_strcasecmp("text/plain", top->subtype) != 0) &&
            (mutt_strcasecmp("application/postscript", top->subtype) != 0))
        {
          if (!mutt_can_decode(top))
          {
            mutt_error(_("I don't know how to print %s attachments!"), type);
            return 0;
          }
        }
      }
    }
    else if (top->parts)
      return (can_print(top->parts, tag));
    if (!tag)
      break;
  }
  return 1;
}

static void print_attachment_list(FILE *fp, int tag, struct Body *top, struct State *state)
{
  char type[STRING];

  for (; top; top = top->next)
  {
    if (!tag || top->tagged)
    {
      snprintf(type, sizeof(type), "%s/%s", TYPE(top), top->subtype);
      if (!option(OPT_ATTACH_SPLIT) && !rfc1524_mailcap_lookup(top, type, NULL, MUTT_PRINT))
      {
        if ((mutt_strcasecmp("text/plain", top->subtype) == 0) ||
            (mutt_strcasecmp("application/postscript", top->subtype) == 0))
          pipe_attachment(fp, top, state);
        else if (mutt_can_decode(top))
        {
          /* decode and print */

          char newfile[_POSIX_PATH_MAX] = "";
          FILE *ifp = NULL;

          mutt_mktemp(newfile, sizeof(newfile));
          if (mutt_decode_save_attachment(fp, top, newfile, MUTT_PRINTING, 0) == 0)
          {
            if ((ifp = fopen(newfile, "r")) != NULL)
            {
              mutt_copy_stream(ifp, state->fpout);
              safe_fclose(&ifp);
              if (AttachSep)
                state_puts(AttachSep, state);
            }
          }
          mutt_unlink(newfile);
        }
      }
      else
        mutt_print_attachment(fp, top);
    }
    else if (top->parts)
      print_attachment_list(fp, tag, top->parts, state);
    if (!tag)
      return;
  }
}

void mutt_print_attachment_list(FILE *fp, int tag, struct Body *top)
{
  struct State state;

  pid_t thepid;
  if (query_quadoption(OPT_PRINT,
                       tag ? _("Print tagged attachment(s)?") :
                             _("Print attachment?")) != MUTT_YES)
    return;

  if (!option(OPT_ATTACH_SPLIT))
  {
    if (!can_print(top, tag))
      return;
    mutt_endwin(NULL);
    memset(&state, 0, sizeof(struct State));
    thepid = mutt_create_filter(NONULL(PrintCmd), &state.fpout, NULL, NULL);
    print_attachment_list(fp, tag, top, &state);
    safe_fclose(&state.fpout);
    if (mutt_wait_filter(thepid) != 0 || option(OPT_WAIT_KEY))
      mutt_any_key_to_continue(NULL);
  }
  else
    print_attachment_list(fp, tag, top, &state);
}

static void update_attach_index(struct Body *cur, struct AttachPtr ***idxp,
                                short *idxlen, short *idxmax, struct Menu *menu)
{
  struct AttachPtr **idx = *idxp;
  while (--(*idxlen) >= 0)
    idx[(*idxlen)]->content = NULL;
  *idxlen = 0;

  idx = *idxp = mutt_gen_attach_list(cur, -1, idx, idxlen, idxmax, 0, 0);

  menu->max = *idxlen;
  menu->data = *idxp;

  if (menu->current >= menu->max)
    menu->current = menu->max - 1;
  menu_check_recenter(menu);
  menu->redraw |= REDRAW_INDEX;
}

int mutt_attach_display_loop(struct Menu *menu, int op, FILE *fp, struct Header *hdr,
                             struct Body *cur, struct AttachPtr ***idxp,
                             short *idxlen, short *idxmax, int recv)
{
  struct AttachPtr **idx = *idxp;

  do
  {
    switch (op)
    {
      case OP_DISPLAY_HEADERS:
        toggle_option(OPT_WEED);
      /* fall through */

      case OP_VIEW_ATTACH:
        op = mutt_view_attachment(fp, idx[menu->current]->content, MUTT_REGULAR,
                                  hdr, idx, *idxlen);
        break;

      case OP_NEXT_ENTRY:
      case OP_MAIN_NEXT_UNDELETED: /* hack */
        if (menu->current < menu->max - 1)
        {
          menu->current++;
          op = OP_VIEW_ATTACH;
        }
        else
          op = OP_NULL;
        break;
      case OP_PREV_ENTRY:
      case OP_MAIN_PREV_UNDELETED: /* hack */
        if (menu->current > 0)
        {
          menu->current--;
          op = OP_VIEW_ATTACH;
        }
        else
          op = OP_NULL;
        break;
      case OP_EDIT_TYPE:
        /* when we edit the content-type, we should redisplay the attachment
           immediately */
        mutt_edit_content_type(hdr, idx[menu->current]->content, fp);
        if (idxmax)
        {
          update_attach_index(cur, idxp, idxlen, idxmax, menu);
          idx = *idxp;
        }
        op = OP_VIEW_ATTACH;
        break;
      /* functions which are passed through from the pager */
      case OP_CHECK_TRADITIONAL:
        if (!(WithCrypto & APPLICATION_PGP) || (hdr && hdr->security & PGP_TRADITIONAL_CHECKED))
        {
          op = OP_NULL;
          break;
        }
      /* fall through */
      case OP_ATTACH_COLLAPSE:
        if (recv)
          return op;
      /* fall through */
      default:
        op = OP_NULL;
    }
  } while (op != OP_NULL);

  return op;
}

static void attach_collapse(struct Body *b, short collapse, short init, short just_one)
{
  short i;
  for (; b; b = b->next)
  {
    i = init || b->collapsed;
    if (i && option(OPT_DIGEST_COLLAPSE) && b->type == TYPEMULTIPART &&
        (mutt_strcasecmp(b->subtype, "digest") == 0))
      attach_collapse(b->parts, 1, 1, 0);
    else if (b->type == TYPEMULTIPART || mutt_is_message_type(b->type, b->subtype))
      attach_collapse(b->parts, collapse, i, 0);
    b->collapsed = collapse;
    if (just_one)
      return;
  }
}

void mutt_attach_init(struct Body *b)
{
  for (; b; b = b->next)
  {
    b->tagged = false;
    b->collapsed = false;
    if (b->parts)
      mutt_attach_init(b->parts);
  }
}

static const char *Function_not_permitted =
    N_("Function not permitted in attach-message mode.");

#define CHECK_ATTACH                                                           \
  if (option(OPT_ATTACH_MSG))                                                    \
  {                                                                            \
    mutt_flushinp();                                                           \
    mutt_error(_(Function_not_permitted));                                     \
    break;                                                                     \
  }

void mutt_view_attachments(struct Header *hdr)
{
  bool secured = false;
  bool need_secured = false;

  char helpstr[LONG_STRING];
  struct Menu *menu = NULL;
  struct Body *cur = NULL;
  struct Message *msg = NULL;
  FILE *fp = NULL;
  struct AttachPtr **idx = NULL;
  short idxlen = 0;
  short idxmax = 0;
  int flags = 0;
  int op = OP_NULL;

  /* make sure we have parsed this message */
  mutt_parse_mime_message(Context, hdr);

  mutt_message_hook(Context, hdr, MUTT_MESSAGEHOOK);

  if ((msg = mx_open_message(Context, hdr->msgno)) == NULL)
    return;

  if (WithCrypto && ((hdr->security & ENCRYPT) ||
                     (mutt_is_application_smime(hdr->content) & SMIMEOPAQUE)))
  {
    need_secured = true;

    if ((hdr->security & ENCRYPT) && !crypt_valid_passphrase(hdr->security))
    {
      mx_close_message(Context, &msg);
      return;
    }
    if ((WithCrypto & APPLICATION_SMIME) && (hdr->security & APPLICATION_SMIME))
    {
      if (hdr->env)
        crypt_smime_getkeys(hdr->env);

      if (mutt_is_application_smime(hdr->content))
      {
        secured = !crypt_smime_decrypt_mime(msg->fp, &fp, hdr->content, &cur);

        /* S/MIME nesting */
        if ((mutt_is_application_smime(cur) & SMIMEOPAQUE))
        {
          struct Body *_cur = cur;
          FILE *_fp = fp;

          fp = NULL;
          cur = NULL;
          secured = !crypt_smime_decrypt_mime(_fp, &fp, _cur, &cur);

          mutt_free_body(&_cur);
          safe_fclose(&_fp);
        }
      }
      else
        need_secured = false;
    }
    if ((WithCrypto & APPLICATION_PGP) && (hdr->security & APPLICATION_PGP))
    {
      if (mutt_is_multipart_encrypted(hdr->content) ||
          mutt_is_malformed_multipart_pgp_encrypted(hdr->content))
        secured = !crypt_pgp_decrypt_mime(msg->fp, &fp, hdr->content, &cur);
      else
        need_secured = false;
    }

    if (need_secured && !secured)
    {
      mx_close_message(Context, &msg);
      mutt_error(_("Can't decrypt encrypted message!"));
      return;
    }
  }

  if (!WithCrypto || !need_secured)
  {
    fp = msg->fp;
    cur = hdr->content;
  }

  menu = mutt_new_menu(MENU_ATTACH);
  menu->title = _("Attachments");
  menu->make_entry = attach_entry;
  menu->tag = mutt_tag_attach;
  menu->help = mutt_compile_help(helpstr, sizeof(helpstr), MENU_ATTACH, AttachHelp);
  mutt_push_current_menu(menu);

  mutt_attach_init(cur);
  attach_collapse(cur, 0, 1, 0);
  update_attach_index(cur, &idx, &idxlen, &idxmax, menu);

  while (true)
  {
    if (op == OP_NULL)
      op = mutt_menu_loop(menu);
    if (!Context)
      return;
    switch (op)
    {
      case OP_ATTACH_VIEW_MAILCAP:
        mutt_view_attachment(fp, idx[menu->current]->content, MUTT_MAILCAP, hdr, idx, idxlen);
        menu->redraw = REDRAW_FULL;
        break;

      case OP_ATTACH_VIEW_TEXT:
        mutt_view_attachment(fp, idx[menu->current]->content, MUTT_AS_TEXT, hdr, idx, idxlen);
        menu->redraw = REDRAW_FULL;
        break;

      case OP_DISPLAY_HEADERS:
      case OP_VIEW_ATTACH:
        op = mutt_attach_display_loop(menu, op, fp, hdr, cur, &idx, &idxlen, &idxmax, 1);
        menu->redraw = REDRAW_FULL;
        continue;

      case OP_ATTACH_COLLAPSE:
        if (!idx[menu->current]->content->parts)
        {
          mutt_error(_("There are no subparts to show!"));
          break;
        }
        if (!idx[menu->current]->content->collapsed)
          attach_collapse(idx[menu->current]->content, 1, 0, 1);
        else
          attach_collapse(idx[menu->current]->content, 0, 1, 1);
        update_attach_index(cur, &idx, &idxlen, &idxmax, menu);
        break;

      case OP_FORGET_PASSPHRASE:
        crypt_forget_passphrase();
        break;

      case OP_EXTRACT_KEYS:
        if ((WithCrypto & APPLICATION_PGP))
        {
          crypt_pgp_extract_keys_from_attachment_list(
              fp, menu->tagprefix, menu->tagprefix ? cur : idx[menu->current]->content);
          menu->redraw = REDRAW_FULL;
        }
        break;

      case OP_CHECK_TRADITIONAL:
        if ((WithCrypto & APPLICATION_PGP) &&
            crypt_pgp_check_traditional(
                fp, menu->tagprefix ? cur : idx[menu->current]->content, menu->tagprefix))
        {
          hdr->security = crypt_query(cur);
          menu->redraw = REDRAW_FULL;
        }
        break;

      case OP_PRINT:
        mutt_print_attachment_list(fp, menu->tagprefix,
                                   menu->tagprefix ? cur : idx[menu->current]->content);
        break;

      case OP_PIPE:
        mutt_pipe_attachment_list(fp, menu->tagprefix,
                                  menu->tagprefix ? cur : idx[menu->current]->content, 0);
        break;

      case OP_SAVE:
        mutt_save_attachment_list(fp, menu->tagprefix,
                                  menu->tagprefix ? cur : idx[menu->current]->content,
                                  hdr, menu);

        if (!menu->tagprefix && option(OPT_RESOLVE) && menu->current < menu->max - 1)
          menu->current++;

        menu->redraw = REDRAW_MOTION_RESYNCH | REDRAW_FULL;
        break;

      case OP_DELETE:
        CHECK_READONLY;

#ifdef USE_POP
        if (Context->magic == MUTT_POP)
        {
          mutt_flushinp();
          mutt_error(_("Can't delete attachment from POP server."));
          break;
        }
#endif

#ifdef USE_NNTP
        if (Context->magic == MUTT_NNTP)
        {
          mutt_flushinp();
          mutt_error(_("Can't delete attachment from news server."));
          break;
        }
#endif

        if (WithCrypto && (hdr->security & ENCRYPT))
        {
          mutt_message(_("Deletion of attachments from encrypted messages is "
                         "unsupported."));
          break;
        }
        if (WithCrypto && (hdr->security & (SIGN | PARTSIGN)))
        {
          mutt_message(_("Deletion of attachments from signed messages may "
                         "invalidate the signature."));
        }
        if (!menu->tagprefix)
        {
          if (idx[menu->current]->parent_type == TYPEMULTIPART)
          {
            idx[menu->current]->content->deleted = true;
            if (option(OPT_RESOLVE) && menu->current < menu->max - 1)
            {
              menu->current++;
              menu->redraw = REDRAW_MOTION_RESYNCH;
            }
            else
              menu->redraw = REDRAW_CURRENT;
          }
          else
            mutt_message(
                _("Only deletion of multipart attachments is supported."));
        }
        else
        {
          int x;

          for (x = 0; x < menu->max; x++)
          {
            if (idx[x]->content->tagged)
            {
              if (idx[x]->parent_type == TYPEMULTIPART)
              {
                idx[x]->content->deleted = true;
                menu->redraw = REDRAW_INDEX;
              }
              else
                mutt_message(
                    _("Only deletion of multipart attachments is supported."));
            }
          }
        }
        break;

      case OP_UNDELETE:
        CHECK_READONLY;
        if (!menu->tagprefix)
        {
          idx[menu->current]->content->deleted = false;
          if (option(OPT_RESOLVE) && menu->current < menu->max - 1)
          {
            menu->current++;
            menu->redraw = REDRAW_MOTION_RESYNCH;
          }
          else
            menu->redraw = REDRAW_CURRENT;
        }
        else
        {
          int x;

          for (x = 0; x < menu->max; x++)
          {
            if (idx[x]->content->tagged)
            {
              idx[x]->content->deleted = false;
              menu->redraw = REDRAW_INDEX;
            }
          }
        }
        break;

      case OP_RESEND:
        CHECK_ATTACH;
        mutt_attach_resend(fp, hdr, idx, idxlen,
                           menu->tagprefix ? NULL : idx[menu->current]->content);
        menu->redraw = REDRAW_FULL;
        break;

      case OP_BOUNCE_MESSAGE:
        CHECK_ATTACH;
        mutt_attach_bounce(fp, hdr, idx, idxlen,
                           menu->tagprefix ? NULL : idx[menu->current]->content);
        menu->redraw = REDRAW_FULL;
        break;

      case OP_FORWARD_MESSAGE:
        CHECK_ATTACH;
        mutt_attach_forward(fp, hdr, idx, idxlen,
                            menu->tagprefix ? NULL : idx[menu->current]->content, 0);
        menu->redraw = REDRAW_FULL;
        break;

#ifdef USE_NNTP
      case OP_FORWARD_TO_GROUP:
        CHECK_ATTACH;
        mutt_attach_forward(fp, hdr, idx, idxlen,
                            menu->tagprefix ? NULL : idx[menu->current]->content, SENDNEWS);
        menu->redraw = REDRAW_FULL;
        break;

      case OP_FOLLOWUP:
        CHECK_ATTACH;

        if (!idx[menu->current]->content->hdr->env->followup_to ||
            (mutt_strcasecmp(idx[menu->current]->content->hdr->env->followup_to, "poster") != 0) ||
            query_quadoption(OPT_FOLLOW_UP_TO_POSTER,
                             _("Reply by mail as poster prefers?")) != MUTT_YES)
        {
          mutt_attach_reply(fp, hdr, idx, idxlen,
                            menu->tagprefix ? NULL : idx[menu->current]->content,
                            SENDNEWS | SENDREPLY);
          menu->redraw = REDRAW_FULL;
          break;
        }
#endif

      case OP_REPLY:
      case OP_GROUP_REPLY:
      case OP_LIST_REPLY:

        CHECK_ATTACH;

        flags = SENDREPLY | (op == OP_GROUP_REPLY ? SENDGROUPREPLY : 0) |
                (op == OP_LIST_REPLY ? SENDLISTREPLY : 0);
        mutt_attach_reply(fp, hdr, idx, idxlen,
                          menu->tagprefix ? NULL : idx[menu->current]->content, flags);
        menu->redraw = REDRAW_FULL;
        break;

      case OP_EDIT_TYPE:
        mutt_edit_content_type(hdr, idx[menu->current]->content, fp);
        update_attach_index(cur, &idx, &idxlen, &idxmax, menu);
        break;

      case OP_EXIT:
        mx_close_message(Context, &msg);
        hdr->attach_del = false;
        while (idxmax-- > 0)
        {
          if (!idx[idxmax])
            continue;
          if (idx[idxmax]->content && idx[idxmax]->content->deleted)
            hdr->attach_del = true;
          if (idx[idxmax]->content)
            idx[idxmax]->content->aptr = NULL;
          FREE(&idx[idxmax]->tree);
          FREE(&idx[idxmax]);
        }
        if (hdr->attach_del)
          hdr->changed = true;
        FREE(&idx);
        idxmax = 0;

        if (WithCrypto && need_secured && secured)
        {
          safe_fclose(&fp);
          mutt_free_body(&cur);
        }

        mutt_pop_current_menu(menu);
        mutt_menu_destroy(&menu);
        return;
    }

    op = OP_NULL;
  }

  /* not reached */
}
