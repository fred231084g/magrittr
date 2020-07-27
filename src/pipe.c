#include <stdbool.h>
#define R_NO_REMAP
#include <Rinternals.h>
#include <R_ext/Visibility.h>
#include "utils.h"

#define export attribute_visible extern


enum pipe_kind {
  PIPE_KIND_none = 0,
  PIPE_KIND_magrittr,
  PIPE_KIND_compound,
  PIPE_KIND_tee,
  PIPE_KIND_dollar
};

// Helper structures for unwind-protection of `.` restoration
struct pipe_info {
  SEXP exprs;
  SEXP env;
};
struct cleanup_info {
  SEXP old;
  SEXP env;
};

// Initialised at load time
static SEXP magrittr_ns_env = NULL;
static SEXP syms_lhs = NULL;
static SEXP syms_rhs = NULL;
static SEXP syms_kind = NULL;
static SEXP syms_env = NULL;
static SEXP syms_lazy = NULL;

static SEXP syms_assign = NULL;
static SEXP syms_curly = NULL;
static SEXP syms_dot = NULL;
static SEXP syms_new_lambda = NULL;
static SEXP syms_paren = NULL;
static SEXP syms_pipe = NULL;
static SEXP syms_pipe_compound = NULL;
static SEXP syms_pipe_dollar = NULL;
static SEXP syms_pipe_lazy = NULL;
static SEXP syms_pipe_tee = NULL;

static SEXP calls_base_with = NULL;
static SEXP chrs_dot = NULL;

static void clean_pipe(void* data);
static SEXP eval_pipe(void* data);
static SEXP eval_pipe_lazy(SEXP exprs, SEXP env);
static SEXP pipe_unroll(SEXP lhs, SEXP rhs, SEXP env, enum pipe_kind kind, SEXP* p_assign);
static SEXP as_pipe_call(SEXP x);
static SEXP add_dot(SEXP x);
static inline SEXP as_pipe_tee_call(SEXP x);
static inline SEXP as_pipe_dollar_call(SEXP x);
SEXP new_lambda(SEXP exprs, SEXP env);

// [[ register() ]]
SEXP magrittr_pipe(SEXP call, SEXP op, SEXP args, SEXP rho) {
  args = CDR(args);

  SEXP lhs = PROTECT(Rf_eval(syms_lhs, rho));
  SEXP rhs = PROTECT(Rf_eval(syms_rhs, rho));
  SEXP kind = PROTECT(Rf_eval(syms_kind, rho));
  SEXP env = PROTECT(Rf_eval(syms_env, rho));

  enum pipe_kind c_kind = INTEGER(kind)[0];
  SEXP assign = R_NilValue;
  SEXP exprs = PROTECT(pipe_unroll(lhs, rhs, env, c_kind, &assign));

  // Create a magrittr lambda when first expression is a `.`
  if (CAR(exprs) == syms_dot) {
    SEXP lambda = new_lambda(CDR(exprs), env);
    UNPROTECT(5);
    return lambda;
  }

  bool use_lazy = Rf_findVar(syms_lazy, rho) != R_UnboundValue;
  SEXP out = R_NilValue;

  if (use_lazy) {
    out = eval_pipe_lazy(exprs, env);
  } else {
    SEXP old = PROTECT(Rf_findVar(syms_dot, env));

    struct pipe_info pipe_info = {
      .exprs = exprs,
      .env = env
    };
    struct cleanup_info cleanup_info = {
      .old = old,
      .env = env
    };

    out =  R_ExecWithCleanup(eval_pipe, &pipe_info, &clean_pipe, &cleanup_info);
    UNPROTECT(1);
  }

  if (assign != R_NilValue) {
    PROTECT(out);
    SEXP call = PROTECT(Rf_lang3(syms_assign, assign, out));
    Rf_eval(call, env);
    UNPROTECT(2);
  }

  UNPROTECT(5);
  return out;
}

static
SEXP eval_pipe(void* data) {
  struct pipe_info* info = (struct pipe_info*) data;

  SEXP exprs = info->exprs;
  SEXP env = info->env;

  SEXP out = R_NilValue;
  while (exprs != R_NilValue) {
    out = PROTECT(Rf_eval(CAR(exprs), env));

    Rf_defineVar(syms_dot, out, env);
    UNPROTECT(1);

    exprs = CDR(exprs);
  }

  return out;
}

static
SEXP eval_pipe_lazy(SEXP exprs, SEXP env) {
  SEXP prev_mask = env;

  // Older masks are protected by newer masks
  PROTECT_INDEX mask_pi;
  PROTECT_WITH_INDEX(R_NilValue, &mask_pi);

  SEXP rest = exprs;
  while ((rest = CDR(exprs)) != R_NilValue) {
    SEXP mask = r_new_environment(prev_mask, 1);
    REPROTECT(mask, mask_pi);

    r_env_bind_lazy(mask, syms_dot, CAR(exprs), prev_mask);

    exprs = rest;
    prev_mask = mask;
  }

  // Evaluate last expression in the very last mask. This triggers a
  // recursive evaluation of `.` bindings in the hierarchy of masks.
  SEXP out = Rf_eval(CAR(exprs), prev_mask);

  UNPROTECT(1);
  return out;
}

static
void clean_pipe(void* data) {
  struct cleanup_info* info = (struct cleanup_info*) data;

  if (info->old == R_UnboundValue) {
    r_env_unbind(info->env, syms_dot);
  } else {
    Rf_defineVar(syms_dot, info->old, info->env);
  }
}


static enum pipe_kind parse_pipe_call(SEXP x);

static
SEXP pipe_unroll(SEXP lhs,
                 SEXP rhs,
                 SEXP env,
                 enum pipe_kind kind,
                 SEXP* p_assign) {
  PROTECT_INDEX out_pi;
  SEXP out = R_NilValue;
  PROTECT_WITH_INDEX(out, &out_pi);

  PROTECT_INDEX rhs_pi;
  PROTECT_WITH_INDEX(rhs, &rhs_pi);

  while (true) {
    if (kind != PIPE_KIND_dollar && TYPEOF(rhs) == LANGSXP && CAR(rhs) == syms_paren) {
      rhs = Rf_eval(rhs, env);
      REPROTECT(rhs, rhs_pi);
    }

    switch (kind) {
    case PIPE_KIND_compound: {
      // Technically we want to give `%<>%` the same precedence as `<-`.
      // In practice, since we only support one top-level `%<>%, we
      // can just interpret it as `%>%` and communicate the assignment
      // variable via `p_assign`.
      *p_assign = lhs;
      rhs = as_pipe_call(rhs);
      break;
    }
    case PIPE_KIND_magrittr: rhs = as_pipe_call(rhs); break;
    case PIPE_KIND_tee: rhs = as_pipe_tee_call(rhs); break;
    case PIPE_KIND_dollar: rhs = as_pipe_dollar_call(rhs); break;
    case PIPE_KIND_none: Rf_error("Internal error in `pipe_unroll()`: Unexpected state.");
    }

    out = Rf_cons(rhs, out);
    REPROTECT(out, out_pi);

    SEXP args = CDR(lhs);

    if ((kind = parse_pipe_call(lhs))) {
      lhs = CAR(args);
      rhs = CADR(args);
      continue;
    }

    break;
  }

  out = Rf_cons(lhs, out);

  UNPROTECT(2);
  return out;
}

static
enum pipe_kind parse_pipe_call(SEXP x) {
  if (TYPEOF(x) != LANGSXP) {
    return PIPE_KIND_none;
  }

  SEXP car = CAR(x);

  if (car == syms_pipe || car == syms_pipe_lazy) {
    return PIPE_KIND_magrittr;
  }
  if (car == syms_pipe_compound) {
    return PIPE_KIND_compound;
  }
  if (car == syms_pipe_tee) {
    return PIPE_KIND_tee;
  }
  if (car == syms_pipe_dollar) {
    return PIPE_KIND_dollar;
  }

  return PIPE_KIND_none;
}

static
SEXP as_pipe_call(SEXP x) {
  // Transform `foo` into `foo()`
  if (TYPEOF(x) != LANGSXP) {
    x = Rf_lcons(x, R_NilValue);
  }
  PROTECT(x);

  // Transform `foo()` into `foo(.)`
  x = add_dot(x);

  UNPROTECT(1);
  return x;
}

static inline
SEXP as_pipe_dollar_call(SEXP x) {
  return Rf_lang3(calls_base_with, syms_dot, x);
}

static inline
SEXP as_pipe_tee_call(SEXP x) {
  x = PROTECT(as_pipe_call(x));
  SEXP out = Rf_lang3(syms_curly, x, syms_dot);

  UNPROTECT(1);
  return out;
}

static
SEXP add_dot(SEXP x) {
  if (TYPEOF(x) != LANGSXP) {
    return x;
  }

  SEXP args = CDR(x);
  while (args != R_NilValue) {
    if (CAR(args) == syms_dot) {
      return x;
    }
    args = CDR(args);
  }

  return Rf_lcons(CAR(x), Rf_cons(syms_dot, CDR(x)));
}


SEXP new_lambda(SEXP exprs, SEXP env) {
  SEXP call = PROTECT(Rf_lang3(syms_new_lambda, exprs, env));
  SEXP out = Rf_eval(call, magrittr_ns_env);

  UNPROTECT(1);
  return out;
}


// Initialisation ----------------------------------------------------

void magrittr_init_utils(SEXP ns);

SEXP magrittr_init(SEXP ns) {
  magrittr_ns_env = ns;
  magrittr_init_utils(ns);

  syms_lhs = Rf_install("lhs");
  syms_rhs = Rf_install("rhs");
  syms_kind = Rf_install("kind");
  syms_env = Rf_install("env");
  syms_lazy = Rf_install("lazy");

  syms_assign = Rf_install("<-");
  syms_curly = Rf_install("{");
  syms_dot = Rf_install(".");
  syms_new_lambda = Rf_install("new_lambda");
  syms_paren = Rf_install("(");
  syms_pipe = Rf_install("%>%");
  syms_pipe_compound = Rf_install("%<>%");
  syms_pipe_dollar = Rf_install("%$%");
  syms_pipe_lazy = Rf_install("%)%");
  syms_pipe_tee = Rf_install("%T>%");

  chrs_dot = Rf_allocVector(STRSXP, 1);
  R_PreserveObject(chrs_dot);
  SET_STRING_ELT(chrs_dot, 0, Rf_mkChar("."));

  calls_base_with = Rf_lang3(Rf_install("::"),
                             Rf_install("base"),
                             Rf_install("with"));
  R_PreserveObject(calls_base_with);
  MARK_NOT_MUTABLE(calls_base_with);

  return R_NilValue;
}

static const R_CallMethodDef call_entries[] = {
  {"magrittr_init",              (DL_FUNC) magrittr_init, 1},
  {NULL, NULL, 0}
};

static const R_ExternalMethodDef ext_entries[] = {
  {"magrittr_pipe",              (DL_FUNC) magrittr_pipe, 0},
  {NULL, NULL, 0}
};

export void R_init_magrittr(DllInfo *dll) {
    R_registerRoutines(dll, NULL, call_entries, NULL, ext_entries);
    R_useDynamicSymbols(dll, FALSE);
}
