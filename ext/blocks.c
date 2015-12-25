#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "cmark_ctype.h"
#include "config.h"
#include "parser.h"
#include "cmark.h"
#include "node.h"
#include "references.h"
#include "utf8.h"
#include "scanners.h"
#include "inlines.h"
#include "houdini.h"
#include "buffer.h"

#define CODE_INDENT 4
#define TAB_STOP 4

#define peek_at(i, n) (i)->data[n]

static inline bool S_is_line_end_char(char c) {
  return (c == '\n' || c == '\r');
}

static void S_parser_feed(cmark_parser *parser, const unsigned char *buffer,
                          size_t len, bool eof);

static void S_process_line(cmark_parser *parser, const unsigned char *buffer,
                           bufsize_t bytes);

static cmark_node *make_block(cmark_node_type tag, int start_line,
                              int start_column) {
  cmark_node *e;

  e = (cmark_node *)calloc(1, sizeof(*e));
  if (e != NULL) {
    e->type = tag;
    e->open = true;
    e->start_line = start_line;
    e->start_column = start_column;
    e->end_line = start_line;
    cmark_strbuf_init(&e->string_content, 32);
  }

  return e;
}

// Create a root document node.
static cmark_node *make_document() {
  cmark_node *e = make_block(CMARK_NODE_DOCUMENT, 1, 1);
  return e;
}

cmark_parser *cmark_parser_new(int options) {
  cmark_parser *parser = (cmark_parser *)malloc(sizeof(cmark_parser));
  cmark_node *document = make_document();
  cmark_strbuf *line = (cmark_strbuf *)malloc(sizeof(cmark_strbuf));
  cmark_strbuf *buf = (cmark_strbuf *)malloc(sizeof(cmark_strbuf));
  cmark_strbuf_init(line, 256);
  cmark_strbuf_init(buf, 0);

  parser->refmap = cmark_reference_map_new();
  parser->root = document;
  parser->current = document;
  parser->line_number = 0;
  parser->offset = 0;
  parser->column = 0;
  parser->first_nonspace = 0;
  parser->first_nonspace_column = 0;
  parser->indent = 0;
  parser->blank = false;
  parser->curline = line;
  parser->last_line_length = 0;
  parser->linebuf = buf;
  parser->options = options;

  return parser;
}

void cmark_parser_free(cmark_parser *parser) {
  cmark_strbuf_free(parser->curline);
  free(parser->curline);
  cmark_strbuf_free(parser->linebuf);
  free(parser->linebuf);
  cmark_reference_map_free(parser->refmap);
  free(parser);
}

static cmark_node *finalize(cmark_parser *parser, cmark_node *b);

// Returns true if line has only space characters, else false.
static bool is_blank(cmark_strbuf *s, bufsize_t offset) {
  while (offset < s->size) {
    switch (s->ptr[offset]) {
    case '\r':
    case '\n':
      return true;
    case ' ':
      offset++;
      break;
    case '\t':
      offset++;
      break;
    default:
      return false;
    }
  }

  return true;
}

static inline bool can_contain(cmark_node_type parent_type,
                               cmark_node_type child_type) {
  return (parent_type == CMARK_NODE_DOCUMENT ||
          parent_type == CMARK_NODE_BLOCK_QUOTE ||
          parent_type == CMARK_NODE_ITEM ||
          (parent_type == CMARK_NODE_LIST && child_type == CMARK_NODE_ITEM));
}

static inline bool accepts_lines(cmark_node_type block_type) {
  return (block_type == CMARK_NODE_PARAGRAPH ||
          block_type == CMARK_NODE_HEADING ||
          block_type == CMARK_NODE_CODE_BLOCK);
}

static void add_line(cmark_node *node, cmark_chunk *ch, bufsize_t offset) {
  assert(node->open);
  cmark_strbuf_put(&node->string_content, ch->data + offset, ch->len - offset);
}

static void remove_trailing_blank_lines(cmark_strbuf *ln) {
  bufsize_t i;
  unsigned char c;

  for (i = ln->size - 1; i >= 0; --i) {
    c = ln->ptr[i];

    if (c != ' ' && c != '\t' && !S_is_line_end_char(c))
      break;
  }

  if (i < 0) {
    cmark_strbuf_clear(ln);
    return;
  }

  for (; i < ln->size; ++i) {
    c = ln->ptr[i];

    if (!S_is_line_end_char(c))
      continue;

    cmark_strbuf_truncate(ln, i);
    break;
  }
}

// Check to see if a node ends with a blank line, descending
// if needed into lists and sublists.
static bool ends_with_blank_line(cmark_node *node) {
  cmark_node *cur = node;
  while (cur != NULL) {
    if (cur->last_line_blank) {
      return true;
    }
    if (cur->type == CMARK_NODE_LIST || cur->type == CMARK_NODE_ITEM) {
      cur = cur->last_child;
    } else {
      cur = NULL;
    }
  }
  return false;
}

// Break out of all containing lists
static int break_out_of_lists(cmark_parser *parser, cmark_node **bptr) {
  cmark_node *container = *bptr;
  cmark_node *b = parser->root;
  // find first containing NODE_LIST:
  while (b && b->type != CMARK_NODE_LIST) {
    b = b->last_child;
  }
  if (b) {
    while (container && container != b) {
      container = finalize(parser, container);
    }
    finalize(parser, b);
    *bptr = b->parent;
  }
  return 0;
}

static cmark_node *finalize(cmark_parser *parser, cmark_node *b) {
  bufsize_t pos;
  cmark_node *item;
  cmark_node *subitem;
  cmark_node *parent;

  parent = b->parent;

  assert(b->open); // shouldn't call finalize on closed blocks
  b->open = false;

  if (parser->curline->size == 0) {
    // end of input - line number has not been incremented
    b->end_line = parser->line_number;
    b->end_column = parser->last_line_length;
  } else if (b->type == CMARK_NODE_DOCUMENT ||
             (b->type == CMARK_NODE_CODE_BLOCK && b->as.code.fenced) ||
             (b->type == CMARK_NODE_HEADING && b->as.heading.setext)) {
    b->end_line = parser->line_number;
    b->end_column = parser->curline->size;
    if (b->end_column && parser->curline->ptr[b->end_column - 1] == '\n')
      b->end_column -= 1;
    if (b->end_column && parser->curline->ptr[b->end_column - 1] == '\r')
      b->end_column -= 1;
  } else {
    b->end_line = parser->line_number - 1;
    b->end_column = parser->last_line_length;
  }

  switch (b->type) {
  case CMARK_NODE_PARAGRAPH:
    while (cmark_strbuf_at(&b->string_content, 0) == '[' &&
           (pos = cmark_parse_reference_inline(&b->string_content,
                                               parser->refmap))) {

      cmark_strbuf_drop(&b->string_content, pos);
    }
    if (is_blank(&b->string_content, 0)) {
      // remove blank node (former reference def)
      cmark_node_free(b);
    }
    break;

  case CMARK_NODE_CODE_BLOCK:
    if (!b->as.code.fenced) { // indented code
      remove_trailing_blank_lines(&b->string_content);
      cmark_strbuf_putc(&b->string_content, '\n');
    } else {

      // first line of contents becomes info
      for (pos = 0; pos < b->string_content.size; ++pos) {
        if (S_is_line_end_char(b->string_content.ptr[pos]))
          break;
      }
      assert(pos < b->string_content.size);

      cmark_strbuf tmp = GH_BUF_INIT;
      houdini_unescape_html_f(&tmp, b->string_content.ptr, pos);
      cmark_strbuf_trim(&tmp);
      cmark_strbuf_unescape(&tmp);
      b->as.code.info = cmark_chunk_buf_detach(&tmp);

      if (b->string_content.ptr[pos] == '\r')
        pos += 1;
      if (b->string_content.ptr[pos] == '\n')
        pos += 1;
      cmark_strbuf_drop(&b->string_content, pos);
    }
    b->as.code.literal = cmark_chunk_buf_detach(&b->string_content);
    break;

  case CMARK_NODE_HTML:
    b->as.literal = cmark_chunk_buf_detach(&b->string_content);
    break;

  case CMARK_NODE_LIST:      // determine tight/loose status
    b->as.list.tight = true; // tight by default
    item = b->first_child;

    while (item) {
      // check for non-final non-empty list item ending with blank line:
      if (item->last_line_blank && item->next) {
        b->as.list.tight = false;
        break;
      }
      // recurse into children of list item, to see if there are
      // spaces between them:
      subitem = item->first_child;
      while (subitem) {
        if (ends_with_blank_line(subitem) && (item->next || subitem->next)) {
          b->as.list.tight = false;
          break;
        }
        subitem = subitem->next;
      }
      if (!(b->as.list.tight)) {
        break;
      }
      item = item->next;
    }

    break;

  default:
    break;
  }
  return parent;
}

// Add a node as child of another.  Return pointer to child.
static cmark_node *add_child(cmark_parser *parser, cmark_node *parent,
                             cmark_node_type block_type, int start_column) {
  assert(parent);

  // if 'parent' isn't the kind of node that can accept this child,
  // then back up til we hit a node that can.
  while (!can_contain(parent->type, block_type)) {
    parent = finalize(parser, parent);
  }

  cmark_node *child = make_block(block_type, parser->line_number, start_column);
  child->parent = parent;

  if (parent->last_child) {
    parent->last_child->next = child;
    child->prev = parent->last_child;
  } else {
    parent->first_child = child;
    child->prev = NULL;
  }
  parent->last_child = child;
  return child;
}

// Walk through node and all children, recursively, parsing
// string content into inline content where appropriate.
static void process_inlines(cmark_node *root, cmark_reference_map *refmap,
                            int options) {
  cmark_iter *iter = cmark_iter_new(root);
  cmark_node *cur;
  cmark_event_type ev_type;

  while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cur = cmark_iter_get_node(iter);
    if (ev_type == CMARK_EVENT_ENTER) {
      if (cur->type == CMARK_NODE_PARAGRAPH || cur->type == CMARK_NODE_HEADING) {
        cmark_parse_inlines(cur, refmap, options);
      }
    }
  }

  cmark_iter_free(iter);
}

// Attempts to parse a list item marker (bullet or enumerated).
// On success, returns length of the marker, and populates
// data with the details.  On failure, returns 0.
static bufsize_t parse_list_marker(cmark_chunk *input, bufsize_t pos,
                                   cmark_list **dataptr) {
  unsigned char c;
  bufsize_t startpos;
  cmark_list *data;

  startpos = pos;
  c = peek_at(input, pos);

  if (c == '*' || c == '-' || c == '+') {
    pos++;
    if (!cmark_isspace(peek_at(input, pos))) {
      return 0;
    }
    data = (cmark_list *)calloc(1, sizeof(*data));
    if (data == NULL) {
      return 0;
    } else {
      data->marker_offset = 0; // will be adjusted later
      data->list_type = CMARK_BULLET_LIST;
      data->bullet_char = c;
      data->start = 1;
      data->delimiter = CMARK_PERIOD_DELIM;
      data->tight = false;
    }
  } else if (cmark_isdigit(c)) {
    int start = 0;
    int digits = 0;

    do {
      start = (10 * start) + (peek_at(input, pos) - '0');
      pos++;
      digits++;
      // We limit to 9 digits to avoid overflow,
      // assuming max int is 2^31 - 1
      // This also seems to be the limit for 'start' in some browsers.
    } while (digits < 9 && cmark_isdigit(peek_at(input, pos)));

    c = peek_at(input, pos);
    if (c == '.' || c == ')') {
      pos++;
      if (!cmark_isspace(peek_at(input, pos))) {
        return 0;
      }
      data = (cmark_list *)calloc(1, sizeof(*data));
      if (data == NULL) {
        return 0;
      } else {
        data->marker_offset = 0; // will be adjusted later
        data->list_type = CMARK_ORDERED_LIST;
        data->bullet_char = 0;
        data->start = start;
        data->delimiter = (c == '.' ? CMARK_PERIOD_DELIM : CMARK_PAREN_DELIM);
        data->tight = false;
      }
    } else {
      return 0;
    }

  } else {
    return 0;
  }

  *dataptr = data;
  return (pos - startpos);
}

// Return 1 if list item belongs in list, else 0.
static int lists_match(cmark_list *list_data, cmark_list *item_data) {
  return (list_data->list_type == item_data->list_type &&
          list_data->delimiter == item_data->delimiter &&
          // list_data->marker_offset == item_data.marker_offset &&
          list_data->bullet_char == item_data->bullet_char);
}

static cmark_node *finalize_document(cmark_parser *parser) {
  while (parser->current != parser->root) {
    parser->current = finalize(parser, parser->current);
  }

  finalize(parser, parser->root);
  process_inlines(parser->root, parser->refmap, parser->options);

  return parser->root;
}

cmark_node *cmark_parse_file(FILE *f, int options) {
  unsigned char buffer[4096];
  cmark_parser *parser = cmark_parser_new(options);
  size_t bytes;
  cmark_node *document;

  while ((bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    bool eof = bytes < sizeof(buffer);
    S_parser_feed(parser, buffer, bytes, eof);
    if (eof) {
      break;
    }
  }

  document = cmark_parser_finish(parser);
  cmark_parser_free(parser);
  return document;
}

cmark_node *cmark_parse_document(const char *buffer, size_t len, int options) {
  cmark_parser *parser = cmark_parser_new(options);
  cmark_node *document;

  S_parser_feed(parser, (const unsigned char *)buffer, len, true);

  document = cmark_parser_finish(parser);
  cmark_parser_free(parser);
  return document;
}

void cmark_parser_feed(cmark_parser *parser, const char *buffer, size_t len) {
  S_parser_feed(parser, (const unsigned char *)buffer, len, false);
}

static void S_parser_feed(cmark_parser *parser, const unsigned char *buffer,
                          size_t len, bool eof) {
  const unsigned char *end = buffer + len;
  static const uint8_t repl[] = {239, 191, 189};

  while (buffer < end) {
    const unsigned char *eol;
    bufsize_t chunk_len;
    bool process = false;
    for (eol = buffer; eol < end; ++eol) {
      if (S_is_line_end_char(*eol)) {
        process = true;
        break;
      }
      if (*eol == '\0' && eol < end) {
        break;
      }
    }
    if (eol >= end && eof) {
      process = true;
    }

    chunk_len = cmark_strbuf_check_bufsize(eol - buffer);
    if (process) {
      if (parser->linebuf->size > 0) {
        cmark_strbuf_put(parser->linebuf, buffer, chunk_len);
        S_process_line(parser, parser->linebuf->ptr, parser->linebuf->size);
        cmark_strbuf_clear(parser->linebuf);
      } else {
        S_process_line(parser, buffer, chunk_len);
      }
    } else {
      if (eol < end && *eol == '\0') {
        // omit NULL byte
        cmark_strbuf_put(parser->linebuf, buffer, chunk_len);
        // add replacement character
        cmark_strbuf_put(parser->linebuf, repl, 3);
        chunk_len += 1; // so we advance the buffer past NULL
      } else {
        cmark_strbuf_put(parser->linebuf, buffer, chunk_len);
      }
    }

    buffer += chunk_len;
    // skip over line ending characters:
    if (buffer < end && *buffer == '\r')
      buffer++;
    if (buffer < end && *buffer == '\n')
      buffer++;
  }
}

static void chop_trailing_hashtags(cmark_chunk *ch) {
  bufsize_t n, orig_n;

  cmark_chunk_rtrim(ch);
  orig_n = n = ch->len - 1;

  // if string ends in space followed by #s, remove these:
  while (n >= 0 && peek_at(ch, n) == '#')
    n--;

  // Check for a space before the final #s:
  if (n != orig_n && n >= 0 &&
      (peek_at(ch, n) == ' ' || peek_at(ch, n) == '\t')) {
    ch->len = n;
    cmark_chunk_rtrim(ch);
  }
}

static void S_find_first_nonspace(cmark_parser *parser, cmark_chunk *input) {
  char c;
  int chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);

  parser->first_nonspace = parser->offset;
  parser->first_nonspace_column = parser->column;
  while ((c = peek_at(input, parser->first_nonspace))) {
    if (c == ' ') {
      parser->first_nonspace += 1;
      parser->first_nonspace_column += 1;
      chars_to_tab = chars_to_tab - 1;
      if (chars_to_tab == 0) {
        chars_to_tab = TAB_STOP;
      }
    } else if (c == '\t') {
      parser->first_nonspace += 1;
      parser->first_nonspace_column += chars_to_tab;
      chars_to_tab = TAB_STOP;
    } else {
      break;
    }
  }

  parser->indent = parser->first_nonspace_column - parser->column;
  parser->blank = S_is_line_end_char(peek_at(input, parser->first_nonspace));
}

static void S_advance_offset(cmark_parser *parser, cmark_chunk *input,
                             bufsize_t count, bool columns) {
  char c;
  int chars_to_tab;
  while (count > 0 && (c = peek_at(input, parser->offset))) {
    if (c == '\t') {
      chars_to_tab = 4 - (parser->column % TAB_STOP);
      parser->column += chars_to_tab;
      parser->offset += 1;
      count -= (columns ? chars_to_tab : 1);
    } else {
      parser->offset += 1;
      parser->column += 1; // assume ascii; block starts are ascii
      count -= 1;
    }
  }
}

static void S_process_line(cmark_parser *parser, const unsigned char *buffer,
                           bufsize_t bytes) {
  cmark_node *last_matched_container;
  bufsize_t matched = 0;
  int lev = 0;
  int i;
  cmark_list *data = NULL;
  bool all_matched = true;
  cmark_node *container;
  bool indented;
  cmark_chunk input;
  bool maybe_lazy;

  if (parser->options & CMARK_OPT_VALIDATE_UTF8) {
    cmark_utf8proc_check(parser->curline, buffer, bytes);
  } else {
    cmark_strbuf_put(parser->curline, buffer, bytes);
  }
  // ensure line ends with a newline:
  if (bytes == 0 || !S_is_line_end_char(parser->curline->ptr[bytes - 1])) {
    cmark_strbuf_putc(parser->curline, '\n');
  }
  parser->offset = 0;
  parser->column = 0;
  parser->blank = false;

  input.data = parser->curline->ptr;
  input.len = parser->curline->size;

  // container starts at the document root.
  container = parser->root;

  parser->line_number++;

  // for each containing node, try to parse the associated line start.
  // bail out on failure:  container will point to the last matching node.

  while (container->last_child && container->last_child->open) {
    container = container->last_child;

    S_find_first_nonspace(parser, &input);

    if (container->type == CMARK_NODE_BLOCK_QUOTE) {
      matched =
          parser->indent <= 3 && peek_at(&input, parser->first_nonspace) == '>';
      if (matched) {
        S_advance_offset(parser, &input, parser->indent + 1, true);
        if (peek_at(&input, parser->offset) == ' ')
          parser->offset++;
      } else {
        all_matched = false;
      }

    } else if (container->type == CMARK_NODE_ITEM) {
      if (parser->indent >=
          container->as.list.marker_offset + container->as.list.padding) {
        S_advance_offset(parser, &input, container->as.list.marker_offset +
                                             container->as.list.padding,
                         true);
      } else if (parser->blank && container->first_child != NULL) {
        // if container->first_child is NULL, then the opening line
        // of the list item was blank after the list marker; in this
        // case, we are done with the list item.
        S_advance_offset(parser, &input,
                         parser->first_nonspace - parser->offset, false);
      } else {
        all_matched = false;
      }

    } else if (container->type == CMARK_NODE_CODE_BLOCK) {

      if (!container->as.code.fenced) { // indented
        if (parser->indent >= CODE_INDENT) {
          S_advance_offset(parser, &input, CODE_INDENT, true);
        } else if (parser->blank) {
          S_advance_offset(parser, &input,
                           parser->first_nonspace - parser->offset, false);
        } else {
          all_matched = false;
        }
      } else { // fenced
        matched = 0;
        if (parser->indent <= 3 && (peek_at(&input, parser->first_nonspace) ==
                                    container->as.code.fence_char)) {
          matched = scan_close_code_fence(&input, parser->first_nonspace);
        }
        if (matched >= container->as.code.fence_length) {
          // closing fence - and since we're at
          // the end of a line, we can return:
          all_matched = false;
          S_advance_offset(parser, &input, matched, false);
          parser->current = finalize(parser, container);
          goto finished;
        } else {
          // skip opt. spaces of fence parser->offset
          i = container->as.code.fence_offset;
          while (i > 0 && peek_at(&input, parser->offset) == ' ') {
            S_advance_offset(parser, &input, 1, false);
            i--;
          }
        }
      }
    } else if (container->type == CMARK_NODE_HEADING) {

      // a heading can never contain more than one line
      all_matched = false;

    } else if (container->type == CMARK_NODE_HTML) {

      switch (container->as.html_block_type) {
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
        // these types of blocks can accept blanks
        break;
      case 6:
      case 7:
        if (parser->blank) {
          all_matched = false;
        }
        break;
      default:
        fprintf(stderr, "Error (%s:%d): Unknown HTML block type %d\n", __FILE__,
                __LINE__, container->as.html_block_type);
        exit(1);
      }

    } else if (container->type == CMARK_NODE_PARAGRAPH) {

      if (parser->blank) {
        all_matched = false;
      }
    }

    if (!all_matched) {
      container = container->parent; // back up to last matching node
      break;
    }
  }

  last_matched_container = container;

  // check to see if we've hit 2nd blank line, break out of list:
  if (parser->blank && container->last_line_blank) {
    break_out_of_lists(parser, &container);
  }

  maybe_lazy = parser->current->type == CMARK_NODE_PARAGRAPH;
  // try new container starts:
  while (container->type != CMARK_NODE_CODE_BLOCK &&
         container->type != CMARK_NODE_HTML) {

    S_find_first_nonspace(parser, &input);
    indented = parser->indent >= CODE_INDENT;

    if (!indented && peek_at(&input, parser->first_nonspace) == '>') {

      S_advance_offset(parser, &input,
                       parser->first_nonspace + 1 - parser->offset, false);
      // optional following character
      if (peek_at(&input, parser->offset) == ' ')
        S_advance_offset(parser, &input, 1, false);
      container = add_child(parser, container, CMARK_NODE_BLOCK_QUOTE,
                            parser->offset + 1);

    } else if (!indented && (matched = scan_atx_heading_start(
                                 &input, parser->first_nonspace))) {

      S_advance_offset(parser, &input,
                       parser->first_nonspace + matched - parser->offset,
                       false);
      container =
          add_child(parser, container, CMARK_NODE_HEADING, parser->offset + 1);

      bufsize_t hashpos =
          cmark_chunk_strchr(&input, '#', parser->first_nonspace);
      int level = 0;

      while (peek_at(&input, hashpos) == '#') {
        level++;
        hashpos++;
      }
      container->as.heading.level = level;
      container->as.heading.setext = false;

    } else if (!indented && (matched = scan_open_code_fence(
                                 &input, parser->first_nonspace))) {

      container = add_child(parser, container, CMARK_NODE_CODE_BLOCK,
                            parser->first_nonspace + 1);
      container->as.code.fenced = true;
      container->as.code.fence_char = peek_at(&input, parser->first_nonspace);
      container->as.code.fence_length = matched;
      container->as.code.fence_offset =
          (int8_t)(parser->first_nonspace - parser->offset);
      container->as.code.info = cmark_chunk_literal("");
      S_advance_offset(parser, &input,
                       parser->first_nonspace + matched - parser->offset,
                       false);

    } else if (!indented && ((matched = scan_html_block_start(
                                  &input, parser->first_nonspace)) ||
                             (container->type != CMARK_NODE_PARAGRAPH &&
                              (matched = scan_html_block_start_7(
                                   &input, parser->first_nonspace))))) {

      container = add_child(parser, container, CMARK_NODE_HTML,
                            parser->first_nonspace + 1);
      container->as.html_block_type = matched;
      // note, we don't adjust parser->offset because the tag is part of the
      // text

    } else if (!indented && container->type == CMARK_NODE_PARAGRAPH &&
               (lev =
                    scan_setext_heading_line(&input, parser->first_nonspace)) &&
               // check that there is only one line in the paragraph:
               (cmark_strbuf_strrchr(
                    &container->string_content, '\n',
                    cmark_strbuf_len(&container->string_content) - 2) < 0)) {

      container->type = CMARK_NODE_HEADING;
      container->as.heading.level = lev;
      container->as.heading.setext = true;
      S_advance_offset(parser, &input, input.len - 1 - parser->offset, false);

    } else if (!indented &&
               !(container->type == CMARK_NODE_PARAGRAPH && !all_matched) &&
               (matched = scan_thematic_break(&input, parser->first_nonspace))) {

      // it's only now that we know the line is not part of a setext heading:
      container = add_child(parser, container, CMARK_NODE_THEMATIC_BREAK,
                            parser->first_nonspace + 1);
      S_advance_offset(parser, &input, input.len - 1 - parser->offset, false);

    } else if ((matched =
                    parse_list_marker(&input, parser->first_nonspace, &data)) &&
               (!indented || container->type == CMARK_NODE_LIST)) {
      // Note that we can have new list items starting with >= 4
      // spaces indent, as long as the list container is still open.

      // compute padding:
      S_advance_offset(parser, &input,
                       parser->first_nonspace + matched - parser->offset,
                       false);
      i = 0;
      while (i <= 5 && peek_at(&input, parser->offset + i) == ' ') {
        i++;
      }
      // i = number of spaces after marker, up to 5
      if (i >= 5 || i < 1 ||
          S_is_line_end_char(peek_at(&input, parser->offset))) {
        data->padding = matched + 1;
        if (i > 0) {
          S_advance_offset(parser, &input, 1, false);
        }
      } else {
        data->padding = matched + i;
        S_advance_offset(parser, &input, i, true);
      }

      // check container; if it's a list, see if this list item
      // can continue the list; otherwise, create a list container.

      data->marker_offset = parser->indent;

      if (container->type != CMARK_NODE_LIST ||
          !lists_match(&container->as.list, data)) {
        container = add_child(parser, container, CMARK_NODE_LIST,
                              parser->first_nonspace + 1);

        memcpy(&container->as.list, data, sizeof(*data));
      }

      // add the list item
      container = add_child(parser, container, CMARK_NODE_ITEM,
                            parser->first_nonspace + 1);
      /* TODO: static */
      memcpy(&container->as.list, data, sizeof(*data));
      free(data);

    } else if (indented && !maybe_lazy && !parser->blank) {
      S_advance_offset(parser, &input, CODE_INDENT, true);
      container = add_child(parser, container, CMARK_NODE_CODE_BLOCK,
                            parser->offset + 1);
      container->as.code.fenced = false;
      container->as.code.fence_char = 0;
      container->as.code.fence_length = 0;
      container->as.code.fence_offset = 0;
      container->as.code.info = cmark_chunk_literal("");

    } else {
      break;
    }

    if (accepts_lines(container->type)) {
      // if it's a line container, it can't contain other containers
      break;
    }
    maybe_lazy = false;
  }

  // what remains at parser->offset is a text line.  add the text to the
  // appropriate container.

  S_find_first_nonspace(parser, &input);

  if (parser->blank && container->last_child) {
    container->last_child->last_line_blank = true;
  }

  // block quote lines are never blank as they start with >
  // and we don't count blanks in fenced code for purposes of tight/loose
  // lists or breaking out of lists.  we also don't set last_line_blank
  // on an empty list item.
  container->last_line_blank =
      (parser->blank && container->type != CMARK_NODE_BLOCK_QUOTE &&
       container->type != CMARK_NODE_HEADING &&
       container->type != CMARK_NODE_THEMATIC_BREAK &&
       !(container->type == CMARK_NODE_CODE_BLOCK &&
         container->as.code.fenced) &&
       !(container->type == CMARK_NODE_ITEM && container->first_child == NULL &&
         container->start_line == parser->line_number));

  cmark_node *cont = container;
  while (cont->parent) {
    cont->parent->last_line_blank = false;
    cont = cont->parent;
  }

  if (parser->current != last_matched_container &&
      container == last_matched_container && !parser->blank &&
      parser->current->type == CMARK_NODE_PARAGRAPH &&
      cmark_strbuf_len(&parser->current->string_content) > 0) {

    add_line(parser->current, &input, parser->offset);

  } else { // not a lazy continuation

    // finalize any blocks that were not matched and set cur to container:
    while (parser->current != last_matched_container) {
      parser->current = finalize(parser, parser->current);
      assert(parser->current != NULL);
    }

    if (container->type == CMARK_NODE_CODE_BLOCK) {

      add_line(container, &input, parser->offset);

    } else if (container->type == CMARK_NODE_HTML) {

      add_line(container, &input, parser->offset);

      int matches_end_condition;
      switch (container->as.html_block_type) {
      case 1:
        // </script>, </style>, </pre>
        matches_end_condition =
            scan_html_block_end_1(&input, parser->first_nonspace);
        break;
      case 2:
        // -->
        matches_end_condition =
            scan_html_block_end_2(&input, parser->first_nonspace);
        break;
      case 3:
        // ?>
        matches_end_condition =
            scan_html_block_end_3(&input, parser->first_nonspace);
        break;
      case 4:
        // >
        matches_end_condition =
            scan_html_block_end_4(&input, parser->first_nonspace);
        break;
      case 5:
        // ]]>
        matches_end_condition =
            scan_html_block_end_5(&input, parser->first_nonspace);
        break;
      default:
        matches_end_condition = 0;
        break;
      }

      if (matches_end_condition) {
        container = finalize(parser, container);
        assert(parser->current != NULL);
      }

    } else if (parser->blank) {

      // ??? do nothing

    } else if (accepts_lines(container->type)) {

      if (container->type == CMARK_NODE_HEADING &&
          container->as.heading.setext == false) {
        chop_trailing_hashtags(&input);
      }
      add_line(container, &input, parser->first_nonspace);

    } else {
      // create paragraph container for line
      container = add_child(parser, container, CMARK_NODE_PARAGRAPH,
                            parser->first_nonspace + 1);
      add_line(container, &input, parser->first_nonspace);
    }

    parser->current = container;
  }
finished:
  parser->last_line_length = parser->curline->size;
  if (parser->last_line_length &&
      parser->curline->ptr[parser->last_line_length - 1] == '\n')
    parser->last_line_length -= 1;
  if (parser->last_line_length &&
      parser->curline->ptr[parser->last_line_length - 1] == '\r')
    parser->last_line_length -= 1;

  cmark_strbuf_clear(parser->curline);
}

cmark_node *cmark_parser_finish(cmark_parser *parser) {
  if (parser->linebuf->size) {
    S_process_line(parser, parser->linebuf->ptr, parser->linebuf->size);
    cmark_strbuf_clear(parser->linebuf);
  }

  finalize_document(parser);

  if (parser->options & CMARK_OPT_NORMALIZE) {
    cmark_consolidate_text_nodes(parser->root);
  }

  cmark_strbuf_free(parser->curline);

#if CMARK_DEBUG_NODES
  if (cmark_node_check(parser->root, stderr)) {
    abort();
  }
#endif
  return parser->root;
}