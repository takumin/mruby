#include "../include/mrc_parser_util.h"
#include "../include/mrc_irep.h"
#include "../include/mrc_ccontext.h"
#include "../include/mrc_codegen.h"
#include "../include/mrc_dump.h"
#include "../include/mrc_codedump.h"
#include "../include/mrc_opcode.h"
#include "../include/mrc_presym.h"
#include "../include/mrc_diagnostic.h"

static mrc_irep *
mrc_load_exec(mrc_ccontext *c, mrc_node *ast)
{
  mrc_irep *irep;
  /* parse error */
  if (0 < c->p->error_list.size) {
    pm_diagnostic_t *e = (pm_diagnostic_t *)c->p->error_list.head;
    while (e) {
      mrc_diagnostic_list_append(c, e->location.start, e->message, MRC_PARSER_ERROR);
      e = (pm_diagnostic_t *)e->node.next;
    }
    return NULL;
  }
  /* parse warning */
  if (0 < c->p->warning_list.size) {
    pm_diagnostic_t *w = (pm_diagnostic_t *)c->p->warning_list.head;
    while (w) {
      mrc_diagnostic_list_append(c, w->location.start, w->message, MRC_PARSER_WARNING);
      w = (pm_diagnostic_t *)w->node.next;
    }
  }
#if defined(MRC_DUMP_PRETTY) && !defined(MRC_NO_STDIO)
  if (c->dump_ast) {
    pm_buffer_t buffer = { 0 };
    pm_prettyprint(&buffer, c->p, ast);
    /* stdout, like the irep dump from mrc_codedump_all(). The buffer is not
       NUL terminated, so it must be written by length. */
    fwrite(pm_buffer_value(&buffer), 1, pm_buffer_length(&buffer), stdout);
    putchar('\n');
    pm_buffer_free(&buffer);
  }
#endif
  irep = mrc_generate_code(c, ast);
  if (c->capture_errors) {
    return NULL;
  }
  if (c->dump_result) {
    mrc_codedump_all(c, irep);
  }

  return irep;
}

static void
partial_hook(void *data, pm_parser_t *p, pm_token_t *token)
{
  mrc_ccontext *c = (mrc_ccontext *)data;
  if (c->current_filename_index + 1 == c->filename_table_length) {
    return;
  }
  uint32_t token_pos = (uint32_t)(token->start - p->start);
  if (token_pos < c->filename_table[c->current_filename_index].start) {
    return;
  }
  if (c->filename_table[c->current_filename_index + 1].start <= token_pos) {
    c->current_filename_index++;
    pm_string_t filename_string;
    pm_string_constant_init(
        &filename_string,
        c->filename_table[c->current_filename_index].filename,
        strlen(c->filename_table[c->current_filename_index].filename));
    p->filepath = filename_string;
  }
}

/* Prism resolves a bare identifier to a local variable only when it knows the
   enclosing scopes, which is what makes `eval` see the locals around it.  The
   host has already handed those scopes down as names, so nothing here reads a
   call frame or a symbol table. */
static void
mrc_pm_options_init(mrc_ccontext *cc)
{
  size_t i, j, scopes_count = 0, scope_index;
  pm_options_t *options;

  if (cc->options) return;
  if (cc->upper_scopes_count == 0) return;

  for (i = 0; i < cc->upper_scopes_count; i++) {
    if (!cc->upper_scopes[i].lvspace) scopes_count++;
  }

  options = (pm_options_t *)mrc_calloc(cc, 1, sizeof(pm_options_t));
  pm_string_constant_init(&options->encoding, "UTF-8", 5);
  pm_options_scopes_init(options, scopes_count + 1); // Prism requires one more scope

  /* Prism reads its scopes outermost first, the table runs innermost first. */
  scope_index = scopes_count;
  for (i = 0; i < cc->upper_scopes_count; i++) {
    const mrc_upper_scope *up = &cc->upper_scopes[i];
    pm_options_scope_t *scope;
    size_t named = 0, local_index = 0;

    if (up->lvspace) continue;
    for (j = 0; j < up->locals_count; j++) {
      if (up->locals[j].name) named++;
    }
    scope = &options->scopes[--scope_index];
    pm_options_scope_init(scope, named);
    for (j = 0; j < up->locals_count; j++) {
      const mrc_upper_local *local = &up->locals[j];

      if (local->name == NULL) continue;
      /* A Prism constant string is a pointer and a length that Prism never
         frees, and the scope table holds these names for as long as the
         context does, so they are shared rather than copied again. */
      pm_string_constant_init(&scope->locals[local_index++], local->name, local->length);
    }
  }

  cc->options = options;
  cc->options_locals_owned = FALSE;
}

static void
mrc_pm_parser_init(mrc_parser_state *p, uint8_t **source, size_t size, mrc_ccontext *cc)
{
  pm_lex_callback_t *cb = (pm_lex_callback_t *)mrc_malloc(cc, sizeof(pm_lex_callback_t));
  cb->data = cc;
  cb->callback = partial_hook;
  mrc_pm_options_init(cc);
  pm_parser_init(p, *source, size, cc->options);
  p->lex_callback = cb;
  mrc_init_presym(&p->constant_pool);
  if (cc->filename_table) {
    pm_string_t filename_string;
    pm_string_constant_init(&filename_string, cc->filename_table[0].filename,
                                             strlen(cc->filename_table[0].filename));
    p->filepath = filename_string;
  }
}

#ifndef MRC_NO_STDIO

#define INITIAL_BUF_SIZE 1024
static intptr_t
append_from_stdin(mrc_ccontext *c, uint8_t **source, size_t source_length)
{
  uint8_t *buffer = (uint8_t *)mrc_malloc(c, INITIAL_BUF_SIZE);
  if (buffer == NULL) return -1;

  size_t capacity = INITIAL_BUF_SIZE;
  size_t length = 0;

  while (1) {
    int ch = getchar();
    if (ch == EOF) {
      buffer[length] = '\0';
      if (*source == NULL)
        *source = (uint8_t *)mrc_malloc(c, source_length + length + 1);
      else
        *source = (uint8_t *)mrc_realloc(c, *source, source_length + length + 1);
      memccpy(*source + source_length, buffer, 1, length);
      mrc_free(c, buffer);
      return length;
    }

    buffer[length++] = (uint8_t)ch;

    if (capacity <= length) {
      capacity *= 2;
      uint8_t *new_buffer = (uint8_t *)mrc_realloc(c, buffer, capacity);
      if (new_buffer == NULL) {
        mrc_free(c, buffer);
        return -1;
      }
      buffer = new_buffer;
    }
  }
}

/* A directory opens for reading on POSIX systems and then fails every read
   with EISDIR, so a stream that opened says nothing about whether it can be
   read.  One byte tells the two apart without asking the platform what kind
   of file this is: an empty file reports end-of-file and no error, while a
   directory raises the error indicator.  The byte is pushed back, so the
   stream is left where it was found.

   The same probe is exported as mrb_stream_is_unreadable() for callers that
   have mruby.h.  This file does not: it is the portable mrc layer, built for
   targets with no mruby core, so it keeps its own copy rather than reach for
   one. */
static int
stream_is_unreadable(FILE *file)
{
  int c = getc(file);
  if (c == EOF) return ferror(file) != 0;
  ungetc(c, file);
  return 0;
}

static intptr_t
read_input_files(mrc_ccontext *c, const char **filenames, uint8_t **source, mrc_filename_table *filename_table)
{
  int i = 0;
  size_t pos = 0;
  intptr_t length = 0;
  intptr_t each_size;
  FILE *file;
  const char *filename = filenames[0];
  while (filename) {
    if (i > 0) {
      /* Separate files with a newline so that a file without a trailing
         newline does not merge its last token with the first token of the
         next file (e.g. `end` + `module` becoming `endmodule`). The separator
         precedes the file content, so filename_table[i].start still points at
         the content and the filename/line mapping is unaffected. See #6907. */
      length += 1;
      if (*source == NULL) {
        *source = (uint8_t *)mrc_malloc(c, length + 1);
      }
      else {
        *source = (uint8_t *)mrc_realloc(c, *source, length + 1);
      }
      (*source)[pos++] = '\n';
      (*source)[length] = '\0';
    }
    filename_table[i].filename = filenames[i];
    filename_table[i].start = pos;
    if (filename[0] == '-' && filename[1] == '\0') {
      each_size = append_from_stdin(c, source, length);
      if (each_size < 0) {
        fprintf(stderr, "compile.c: cannot read from stdin\n");
        return -1;
      }
      length += each_size;
    }
    else {
      file = NULL;
      file = fopen(filename, "rb");
      if (!file) {
        fprintf(stderr, "compile.c: cannot open program file. (%s)\n", filename);
        return -1;
      }
      fseek(file, 0, SEEK_END);
      each_size = ftell(file);
      fseek(file, 0, SEEK_SET);
      if (each_size < 0) {
        /* Not a seekable file (a pipe, FIFO or terminal); its size cannot be
           determined up front, so the read-it-all-at-once path below does not
           apply. */
        fprintf(stderr, "compile.c: cannot get size of program file. (%s)\n", filename);
        fclose(file);
        return -1;
      }
      if (stream_is_unreadable(file)) {
        /* The size above is not trustworthy for a stream that cannot be read:
           a directory answers LONG_MAX to ftell() on ext4 and 0 on tmpfs, so
           it either overflows the length arithmetic below before the
           allocation is attempted, or compiles as an empty program.  The
           wording differs from the fread() failure below so that the two
           cannot be mistaken for each other. */
        fprintf(stderr, "compile.c: cannot read from program file. (%s)\n", filename);
        fclose(file);
        return -1;
      }
      length += each_size;
      if (*source == NULL) {
        *source = (uint8_t *)mrc_malloc(c, length + 1);
      }
      else {
        *source = (uint8_t *)mrc_realloc(c, *source, length + 1);
      }
      if (fread(*source + pos, sizeof(char), (size_t)each_size, file) != (size_t)each_size) {
        fprintf(stderr, "compile.c: cannot read program file. (%s)\n", filename);
        fclose(file);
        return -1;
      }
      fclose(file);
      (*source)[length] = '\0';
    }
    pos += each_size;
    filename = filenames[++i];
  }
  return length;
}

static mrc_node *
mrc_pm_parse(mrc_ccontext *cc)
{
  mrc_node *node = pm_parse(cc->p);

#if defined(PICORB_VM_MRUBYC)
  // Workaround: save top-level locals for PicoRuby(mruby/c) IRB
  pm_program_node_t *program = (pm_program_node_t *)node;
  uint32_t nlocals = program->locals.size;
  pm_options_t *options = (pm_options_t *)mrc_malloc(cc, sizeof(pm_options_t));
  memset(options, 0, sizeof(pm_options_t));
  pm_string_t *encoding = &options->encoding;
  pm_string_constant_init(encoding, "UTF-8", 5);
  pm_options_scopes_init(options, 1);
  pm_options_scope_t *options_scope = &options->scopes[0];
  pm_options_scope_init(options_scope, nlocals);
  pm_constant_id_t id;
  pm_constant_t *local;
  pm_string_t *scope_local;
  char *allocated;
  for (int i = 0; i < nlocals; i++) {
    scope_local = &options_scope->locals[i];
    id = program->locals.ids[i];
    local = pm_constant_pool_id_to_constant(&cc->p->constant_pool, id);
    allocated = (char *)mrc_malloc(cc, local->length);
    memcpy(allocated, local->start, local->length);
    pm_string_constant_init(scope_local, (const char *)allocated, local->length);
  }
  if (cc->options && cc->options->scopes) {
    for (int i = 0; i < cc->options->scopes[0].locals_count; i++) {
      mrc_free(cc, (void *)cc->options->scopes[0].locals[i].source);
    }
    mrc_free(cc, cc->options);
  }
  cc->options = options;
#endif

  return node;
}


static mrc_node *
mrc_parse_file_cxt(mrc_ccontext *c, const char **filenames, uint8_t **source)
{
  size_t filecount = 0;
  while (filenames[filecount]) {
    filecount++;
  }
  c->filename_table = (mrc_filename_table *)mrc_malloc(c, sizeof(mrc_filename_table) * filecount);
  c->filename_table_length = filecount;
  c->current_filename_index = 0;
  intptr_t length = read_input_files(c, filenames, source, c->filename_table);
  if (length < 0) {
    fprintf(stderr, "Cannot open files: ");
    for (size_t i = 0; i < filecount; i++) {
      fprintf(stderr, "%s ", filenames[i]);
    }
    fprintf(stderr, "\n");
    return NULL;
  }
  mrc_pm_parser_init(c->p, source, length, c);
  return mrc_pm_parse(c);
}

MRC_API mrc_irep *
mrc_load_file_cxt(mrc_ccontext *c, const char **filenames, uint8_t **source)
{
  mrc_node *root = mrc_parse_file_cxt(c, filenames, source);
  if (root == NULL) {
    return NULL;
  }
  mrc_irep *irep = mrc_load_exec(c, root);
  pm_node_destroy(c->p, root);
  return irep;
}
#endif

static mrc_node *
mrc_parse_string_cxt(mrc_ccontext *c, const uint8_t **source, size_t length)
{
  c->filename_table = (mrc_filename_table *)mrc_malloc(c, sizeof(mrc_filename_table));
  c->filename_table[0].filename = c->filename ? c->filename : "-e";
  c->filename_table[0].start = 0;
  c->filename_table_length = 1;
  c->current_filename_index = 0;
  mrc_pm_parser_init(c->p, (uint8_t **)source, length, c);
  return mrc_pm_parse(c);
}

MRC_API mrc_irep *
mrc_load_string_cxt(mrc_ccontext *c, const uint8_t **source, size_t length)
{
  mrc_node *root = mrc_parse_string_cxt(c, source, length);
  mrc_irep *irep = mrc_load_exec(c, root);
  pm_node_destroy(c->p, root);
  return irep;
}
