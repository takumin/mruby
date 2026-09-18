/*
 * VM Re-entry Micro-benchmark
 *
 * What a C method pays to run Ruby by re-entering the VM. Ruby alone cannot
 * separate that cost: both forms are reached the same way from bytecode, and
 * they differ only in what happens between the two Ruby frames, so a method
 * written each way has to be put side by side from C. Each group below prints
 * the re-entrant form next to the nearest form that runs the same Ruby without
 * re-entering, and the loop that carries both, which subtracts out.
 *
 * The first group is the question stage 2 asks: what an iterator moved from C
 * to Ruby costs. The second is the one stage 3 asks: what a method call from C
 * costs against the same call from bytecode.
 *
 * Compile:
 *   cc -O2 -I include -I build/host/include \
 *      benchmark/vm_reentry_bench.c \
 *      build/host/lib/libmruby.a -lm -o vm_reentry_bench
 *
 * Run:
 *   ./vm_reentry_bench [iterations] [repeats]
 *
 * Wall clock on CI-class hardware does not resolve a difference under about
 * 5%: the same unmodified binary varies by that much between runs. For
 * anything smaller, count instructions instead, which reproduce exactly:
 *
 *   valgrind --tool=callgrind --callgrind-out-file=/dev/null \
 *     build/host/bin/mruby benchmark/bm_vm_reentry.rb
 *
 * and compare the "I   refs" line between two commits.
 */

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/compile.h>
#include <mruby/proc.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile long sink;

static double
now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* A C iterator that re-enters the VM once per element. */
static mrb_value
ary_each_c(mrb_state *mrb, mrb_value self)
{
  mrb_value blk;
  mrb_get_args(mrb, "&!", &blk);
  mrb_int len = RARRAY_LEN(self);
  int ai = mrb_gc_arena_save(mrb);
  for (mrb_int i = 0; i < len; i++) {
    mrb_yield(mrb, blk, RARRAY_PTR(self)[i]);
    mrb_gc_arena_restore(mrb, ai);
  }
  return self;
}

/* A C loop that calls a Ruby method through mrb_funcall. */
static mrb_value
call_n_funcall(mrb_state *mrb, mrb_value self)
{
  mrb_value recv;
  mrb_int n;
  mrb_get_args(mrb, "oi", &recv, &n);
  int ai = mrb_gc_arena_save(mrb);
  mrb_sym mid = mrb_intern_lit(mrb, "m");
  mrb_value v = mrb_nil_value();
  for (mrb_int i = 0; i < n; i++) {
    v = mrb_funcall_id(mrb, recv, mid, 1, mrb_fixnum_value(i));
    mrb_gc_arena_restore(mrb, ai);
  }
  return v;
}

/* The same loop with the call taken out, to price what carries it. The write
   to `sink` is what keeps the loop from being optimized away whole. */
static mrb_value
call_n_empty(mrb_state *mrb, mrb_value self)
{
  mrb_value recv;
  mrb_int n;
  mrb_get_args(mrb, "oi", &recv, &n);
  int ai = mrb_gc_arena_save(mrb);
  mrb_value v = mrb_nil_value();
  for (mrb_int i = 0; i < n; i++) {
    v = mrb_fixnum_value(i);
    sink += (long)i;
    mrb_gc_arena_restore(mrb, ai);
  }
  return v;
}

/* setjmp alone, since entering the VM sets a jump buffer up. */
static jmp_buf jbuf;

static double
bench_setjmp(long n)
{
  double t0 = now_ns();
  for (long i = 0; i < n; i++) {
    if (setjmp(jbuf) == 0) sink += i;
  }
  return (now_ns() - t0) / (double)n;
}

static void
run(mrb_state *mrb, const char *label, const char *src, long n, int reps)
{
  char buf[512];
  double best = 1e30;

  snprintf(buf, sizeof(buf), src, n);
  for (int r = 0; r < reps; r++) {
    mrbc_context *c = mrbc_context_new(mrb);
    mrbc_filename(mrb, c, "bench");
    double t0 = now_ns();
    mrb_load_string_cxt(mrb, buf, c);
    double dt = now_ns() - t0;
    mrbc_context_free(mrb, c);
    if (mrb->exc) {
      mrb_print_error(mrb);
      mrb->exc = NULL;
      return;
    }
    if (dt < best) best = dt;
  }
  printf("%-46s %10.2f ns/op\n", label, best / (double)n);
}

int
main(int argc, char **argv)
{
  long n = (argc > 1) ? atol(argv[1]) : 3000000;
  int reps = (argc > 2) ? atoi(argv[2]) : 7;
  mrb_state *mrb = mrb_open();

  if (!mrb) {
    fprintf(stderr, "Failed to create mrb_state\n");
    return 1;
  }

  mrb_define_method_id(mrb, mrb->array_class, mrb_intern_lit(mrb, "each_c"),
                       ary_each_c, MRB_ARGS_BLOCK());
  mrb_define_method_id(mrb, mrb->object_class, mrb_intern_lit(mrb, "call_n_funcall"),
                       call_n_funcall, MRB_ARGS_REQ(2));
  mrb_define_method_id(mrb, mrb->object_class, mrb_intern_lit(mrb, "call_n_empty"),
                       call_n_empty, MRB_ARGS_REQ(2));

  mrb_load_string(mrb,
    "class Array\n"
    "  def each_rb\n"
    "    i = 0\n"
    "    n = size\n"
    "    while i < n\n"
    "      yield self[i]\n"
    "      i += 1\n"
    "    end\n"
    "    self\n"
    "  end\n"
    "end\n"
    "class Recv\n"
    "  def m(x); x; end\n"
    "end\n"
    "$recv = Recv.new\n"
    "$a = Array.new(1000) { |i| i }\n");
  if (mrb->exc) {
    mrb_print_error(mrb);
    mrb_close(mrb);
    return 1;
  }

  printf("iterations: %ld, best of %d\n\n", n, reps);

  printf("--- a block, per yielded element ---\n");
  run(mrb, "C iterator, mrb_yield (re-entry)",
      "s=0; k=%ld/1000; k.times { $a.each_c { |x| s += x } }", n, reps);
  run(mrb, "Ruby iterator, yield (no re-entry)",
      "s=0; k=%ld/1000; k.times { $a.each_rb { |x| s += x } }", n, reps);
  run(mrb, "inline while loop (no call at all)",
      "s=0; k=%ld/1000; k.times { i=0; while i<1000; s += $a[i]; i+=1; end }", n, reps);

  printf("\n--- a method call, per call ---\n");
  run(mrb, "mrb_funcall_id from C (re-entry)",
      "call_n_funcall($recv, %ld)", n, reps);
  run(mrb, "the C loop alone (baseline)",
      "call_n_empty($recv, %ld)", n, reps);
  run(mrb, "OP_SEND from bytecode",
      "r=$recv; i=0; n=%ld; while i<n; r.m(i); i+=1; end", n, reps);
  run(mrb, "the bytecode loop alone (baseline)",
      "r=$recv; i=0; n=%ld; while i<n; i+=1; end", n, reps);

  printf("\n--- what a suspend would have to move, per round trip ---\n");
  run(mrb, "Fiber#resume and Fiber.yield",
      "f = Fiber.new { loop { Fiber.yield } }; "
      "i=0; n=%ld; while i<n; f.resume; i+=1; end", n / 10, reps);

  printf("\n--- reference ---\n");
  {
    double best = 1e30;
    for (int r = 0; r < reps; r++) {
      double v = bench_setjmp(n);
      if (v < best) best = v;
    }
    printf("%-46s %10.2f ns/op\n", "setjmp() alone", best);
  }

  mrb_close(mrb);
  return 0;
}
