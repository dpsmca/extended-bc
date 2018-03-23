/*
 * *****************************************************************************
 *
 * Copyright 2018 Gavin D. Howard
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * *****************************************************************************
 *
 * Code to execute bc programs.
 *
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <limits.h>
#include <unistd.h>

#include <program.h>
#include <parse.h>
#include <instructions.h>
#include <bc.h>

BcStatus bc_program_search(BcProgram *p, BcResult *result,
                           BcNum **ret, uint8_t flags)
{
  BcStatus status;
  BcEntry entry, *entry_ptr;
  BcVec *vec;
  BcVecO *veco;
  size_t idx, ip_idx;
  BcAuto *a;
  int var;

  for (ip_idx = 0; ip_idx < p->stack.len - 1; ++ip_idx) {

    BcFunc *func;
    BcInstPtr *ip;

    ip = bc_vec_item_rev(&p->stack, ip_idx);

    assert(ip);

    if (ip->func == BC_PROGRAM_READ || ip->func == BC_PROGRAM_MAIN) continue;

    func = bc_vec_item(&p->funcs, ip->func);

    assert(func);

    for (idx = 0; idx < func->autos.len; ++idx) {

      a = bc_vec_item(&func->autos, idx);
      assert(a);

      if (!strcmp(a->name, result->data.id.name)) {

        BcResult *r;
        uint8_t cond;

        cond = flags & BC_PROGRAM_SEARCH_VAR;

        if (!a->var != !cond) return BC_STATUS_EXEC_BAD_TYPE;

        r = bc_vec_item(&p->results, ip->len + idx);

        assert(r);

        if (cond || flags & BC_PROGRAM_SEARCH_ARRAY) *ret = &r->data.num;
        else {
          status = bc_array_expand(&r->data.array, result->data.id.idx + 1);
          if (status) return status;
          *ret = bc_vec_item(&r->data.array, result->data.id.idx);
        }

        return BC_STATUS_SUCCESS;
      }
    }
  }

  var = flags & BC_PROGRAM_SEARCH_VAR;
  vec = var ? &p->vars : &p->arrays;
  veco = var ? &p->var_map : &p->array_map;

  entry.name = result->data.id.name;
  entry.idx = vec->len;

  status = bc_veco_insert(veco, &entry, &idx);

  if (status != BC_STATUS_VEC_ITEM_EXISTS) {

    // We use this because it has a union of BcNum and BcVec.
    BcResult data;
    size_t len;

    if (status) return status;

    len = strlen(entry.name) + 1;

    if (!(result->data.id.name = malloc(len))) return BC_STATUS_MALLOC_FAIL;

    strcpy(result->data.id.name, entry.name);

    if (flags & BC_PROGRAM_SEARCH_VAR)
      status = bc_num_init(&data.data.num, BC_NUM_DEF_SIZE);
    else status = bc_vec_init(&data.data.array, sizeof(BcNum), bc_num_free);

    if (status) return status;

    if ((status = bc_vec_push(vec, &data.data))) return status;
  }

  entry_ptr = bc_veco_item(veco, idx);

  assert(entry_ptr);

  if (var) {
    *ret = bc_vec_item(vec, entry_ptr->idx);
    assert(*ret);
  }
  else {

    BcVec *aptr = bc_vec_item(vec, entry_ptr->idx);

    assert(aptr);

    if (flags & BC_PROGRAM_SEARCH_ARRAY) {
      *ret = (BcNum*) aptr;
      return BC_STATUS_SUCCESS;
    }

    status = bc_array_expand(aptr, result->data.id.idx + 1);
    if (status) return status;

    *ret = bc_vec_item(aptr, result->data.id.idx);
  }

  return BC_STATUS_SUCCESS;
}

BcStatus bc_program_num(BcProgram *p, BcResult *result, BcNum** num, bool hex) {

  BcStatus status = BC_STATUS_SUCCESS;

  switch (result->type) {

    case BC_RESULT_INTERMEDIATE:
    case BC_RESULT_SCALE:
    {
      *num = &result->data.num;
      break;
    }

    case BC_RESULT_CONSTANT:
    {
      char** s;
      size_t len, base;

      s = bc_vec_item(&p->constants, result->data.id.idx);
      assert(s);
      len = strlen(*s);

      if ((status = bc_num_init(&result->data.num, len))) return status;

      base = hex && len == 1 ? BC_NUM_MAX_INPUT_BASE : p->ibase_t;

      status = bc_num_parse(&result->data.num, *s, &p->ibase, base);

      if (status) {
        bc_num_free(&result->data.num);
        return status;
      }

      *num = &result->data.num;

      result->type = BC_RESULT_INTERMEDIATE;

      break;
    }

    case BC_RESULT_VAR:
    case BC_RESULT_ARRAY:
    {
      uint8_t flags = result->type == BC_RESULT_VAR ? BC_PROGRAM_SEARCH_VAR : 0;
      status = bc_program_search(p, result, num, flags);
      break;
    }

    case BC_RESULT_LAST:
    {
      *num = &p->last;
      break;
    }

    case BC_RESULT_IBASE:
    {
      *num = &p->ibase;
      break;
    }

    case BC_RESULT_OBASE:
    {
      *num = &p->obase;
      break;
    }

    case BC_RESULT_ONE:
    {
      *num = &p->one;
      break;
    }

    default:
    {
      // This is here to prevent compiler warnings in release mode.
      *num = NULL;
      assert(false);
      break;
    }
  }

  return status;
}

BcStatus bc_program_binaryOpPrep(BcProgram *p, BcResult **left, BcNum **lval,
                                 BcResult **right, BcNum **rval)
{
  BcStatus status;
  BcResult *l, *r;
  bool hex;

  assert(p && left && lval && right && rval &&
         BC_PROGRAM_CHECK_RESULTS(p, 2));

  r = bc_vec_item_rev(&p->results, 0);
  l = bc_vec_item_rev(&p->results, 1);

  assert(r && l);

  hex = l->type == BC_RESULT_IBASE || l->type == BC_RESULT_OBASE;

  if ((status = bc_program_num(p, l, lval, false))) return status;
  if ((status = bc_program_num(p, r, rval, hex))) return status;

  *left = l;
  *right = r;

  return BC_STATUS_SUCCESS;
}

BcStatus bc_program_binaryOpRetire(BcProgram *p, BcResult *result,
                                   BcResultType type)
{
  BcStatus status;

  result->type = type;

  if ((status = bc_vec_pop(&p->results))) return status;
  if ((status = bc_vec_pop(&p->results))) return status;

  return bc_vec_push(&p->results, result);
}

BcStatus bc_program_unaryOpPrep(BcProgram *p, BcResult **result, BcNum **val) {

  BcStatus status;
  BcResult *r;

  assert(p && result && val && BC_PROGRAM_CHECK_RESULTS(p, 1));

  r = bc_vec_item_rev(&p->results, 0);

  assert(r);

  if ((status = bc_program_num(p, r, val, false))) return status;

  *result = r;

  return BC_STATUS_SUCCESS;
}

BcStatus bc_program_unaryOpRetire(BcProgram *p, BcResult *result,
                                  BcResultType type)
{
  BcStatus status;
  result->type = type;
  if ((status = bc_vec_pop(&p->results))) return status;
  return bc_vec_push(&p->results, result);
}

BcStatus bc_program_op(BcProgram *p, uint8_t inst) {

  BcStatus status;
  BcResult *operand1, *operand2, result;
  BcNum *num1, *num2;

  status = bc_program_binaryOpPrep(p, &operand1, &num1, &operand2, &num2);
  if (status) return status;

  if ((status = bc_num_init(&result.data.num, BC_NUM_DEF_SIZE))) return status;

  if (inst != BC_INST_OP_POWER) {
    BcNumBinaryFunc op = bc_program_math_ops[inst - BC_INST_OP_MODULUS];
    status = op(num1, num2, &result.data.num, p->scale);
  }
  else status = bc_num_pow(num1, num2, &result.data.num, p->scale);

  if (status) goto err;

  status = bc_program_binaryOpRetire(p, &result, BC_RESULT_INTERMEDIATE);
  if (status) goto err;

  return status;

err:

  bc_num_free(&result.data.num);

  return status;
}

BcStatus bc_program_read(BcProgram *p) {

  BcStatus status;
  BcParse parse;
  char *buffer;
  size_t size;
  BcFunc *func;
  BcInstPtr ip;

  func = bc_vec_item(&p->funcs, BC_PROGRAM_READ);
  assert(func);
  func->code.len = 0;

  if (!(buffer = malloc(BC_PROGRAM_BUF_SIZE + 1))) return BC_STATUS_MALLOC_FAIL;

  size = BC_PROGRAM_BUF_SIZE;

  if (getline(&buffer, &size, stdin) < 0) {
    status = BC_STATUS_IO_ERR;
    goto io_err;
  }

  if ((status = bc_parse_init(&parse, p))) goto io_err;
  bc_lex_init(&parse.lex, "<stdin>");
  if ((status = bc_lex_text(&parse.lex, buffer, &parse.token))) goto exec_err;

  status = bc_parse_expr(&parse, &func->code, BC_PARSE_EXPR_NO_READ);

  if (status != BC_STATUS_LEX_EOF && parse.token.type != BC_LEX_NEWLINE) {
    status = status ? status : BC_STATUS_EXEC_BAD_READ_EXPR;
    goto exec_err;
  }

  ip.func = BC_PROGRAM_READ;
  ip.idx = 0;
  ip.len = p->results.len;

  if ((status = bc_vec_push(&p->stack, &ip))) goto exec_err;
  if ((status = bc_program_exec(p))) goto exec_err;

  status = bc_vec_pop(&p->stack);

exec_err:

  bc_parse_free(&parse);

io_err:

  free(buffer);

  return status;
}

size_t bc_program_index(uint8_t *code, size_t *start) {

  uint8_t bytes, byte, i;
  size_t result;

  bytes = code[(*start)++];
  result = 0;

  for (i = 0; i < bytes; ++i) {
    byte = code[(*start)++];
    result |= (((size_t) byte) << (i * CHAR_BIT));
  }

  return result;
}

char* bc_program_name(uint8_t *code, size_t *start) {

  char byte, *s, *string, *ptr;
  size_t len, i;

  string = (char*) (code + *start);
  ptr = strchr((char*) string, ':');

  if (ptr) len = ((unsigned long) ptr) - ((unsigned long) string);
  else len = strlen(string);

  if (!(s = malloc(len + 1))) return NULL;

  byte = code[(*start)++];
  i = 0;

  while (byte && byte != ':') {
    s[i++] = byte;
    byte = code[(*start)++];
  }

  s[i] = '\0';

  return s;
}

BcStatus bc_program_printIndex(uint8_t *code, size_t *start) {

  uint8_t bytes, byte, i;

  bytes = code[(*start)++];
  byte = 1;

  if (printf(bc_program_byte_fmt, bytes) < 0) return BC_STATUS_IO_ERR;

  for (i = 0; byte && i < bytes; ++i) {
    byte = code[(*start)++];
    if (printf(bc_program_byte_fmt, byte) < 0) return BC_STATUS_IO_ERR;
  }

  return BC_STATUS_SUCCESS;
}

BcStatus bc_program_printName(uint8_t *code, size_t *start) {

  BcStatus status;
  char byte;

  status = BC_STATUS_SUCCESS;
  byte = code[(*start)++];

  while (byte && byte != ':') {
    if (putchar(byte) == EOF) return BC_STATUS_IO_ERR;
    byte = code[(*start)++];
  }

  assert(byte);

  if (putchar(byte) == EOF) status = BC_STATUS_IO_ERR;

  return status;
}

BcStatus bc_program_printString(const char *str, size_t *nchars) {

  char c, c2;
  size_t len, i;
  int err;

  err = 0;
  len = strlen(str);

  for (i = 0; i < len; ++i,  ++(*nchars)) {

    c = str[i];

    if (c != '\\') err = putchar(c);
    else {

      ++i;
      assert(i < len);
      c2 = str[i];

      switch (c2) {

        case 'a':
        {
          err = putchar('\a');
          break;
        }

        case 'b':
        {
          err = putchar('\b');
          break;
        }

        case 'e':
        {
          err = putchar('\\');
          break;
        }

        case 'f':
        {
          err = putchar('\f');
          break;
        }

        case 'n':
        {
          err = putchar('\n');
          *nchars = SIZE_MAX;
          break;
        }

        case 'r':
        {
          err = putchar('\r');
          break;
        }

        case 'q':
        {
          err = putchar('"');
          break;
        }

        case 't':
        {
          err = putchar('\t');
          break;
        }

        default:
        {
          // Do nothing.
          err = 0;
          break;
        }
      }
    }

    if (err == EOF) return BC_STATUS_IO_ERR;
  }

  return BC_STATUS_SUCCESS;
}

BcStatus bc_program_push(BcProgram *p, uint8_t *code, size_t *start, bool var) {

  BcStatus status;
  BcResult result;

  result.data.id.name = bc_program_name(code, start);

  assert(result.data.id.name);

  if (var) {
    result.type = BC_RESULT_VAR;
    status = bc_vec_push(&p->results, &result);
  }
  else {

    BcResult *operand;
    BcNum *num;
    unsigned long temp;

    if ((status = bc_program_unaryOpPrep(p, &operand, &num))) goto err;
    if ((status = bc_num_ulong(num, &temp))) goto err;

    if (temp > (unsigned long) p->dim_max) {
      status = BC_STATUS_EXEC_ARRAY_LEN;
      goto err;
    }

    result.data.id.idx = (size_t) temp;

    status = bc_program_unaryOpRetire(p, &result, BC_RESULT_ARRAY);
  }

  if (status) goto err;

  return status;

err:

  free(result.data.id.name);

  return status;
}

BcStatus bc_program_negate(BcProgram *p) {

  BcStatus status;
  BcResult result;
  BcResult *ptr;
  BcNum *num;

  if ((status = bc_program_unaryOpPrep(p, &ptr, &num))) return status;
  if ((status = bc_num_init(&result.data.num, num->len))) return status;
  if ((status = bc_num_copy(&result.data.num, num))) goto err;

  result.data.num.neg = !result.data.num.neg;

  status = bc_program_unaryOpRetire(p, &result, BC_RESULT_INTERMEDIATE);
  if (status) goto err;

  return status;

err:

  bc_num_free(&result.data.num);

  return status;
}

BcStatus bc_program_logical(BcProgram *p, uint8_t inst) {

  BcStatus status;
  BcResult *operand1, *operand2, result;
  BcNum *num1, *num2;
  BcNumInitFunc init;
  bool cond;
  int cmp;

  status = bc_program_binaryOpPrep(p, &operand1, &num1, &operand2, &num2);
  if (status) return status;

  if ((status = bc_num_init(&result.data.num, BC_NUM_DEF_SIZE))) return status;

  if (inst == BC_INST_OP_BOOL_AND)
    cond = bc_num_cmp(num1, &p->zero, NULL) && bc_num_cmp(num2, &p->zero, NULL);
  else if (inst == BC_INST_OP_BOOL_OR)
    cond = bc_num_cmp(num1, &p->zero, NULL) || bc_num_cmp(num2, &p->zero, NULL);
  else {

    cmp = bc_num_cmp(num1, num2, NULL);

    switch (inst) {
      case BC_INST_OP_REL_EQUAL:
      {
        cond = cmp == 0;
        break;
      }

      case BC_INST_OP_REL_LESS_EQ:
      {
        cond = cmp <= 0;
        break;
      }

      case BC_INST_OP_REL_GREATER_EQ:
      {
        cond = cmp >= 0;
        break;
      }

      case BC_INST_OP_REL_NOT_EQ:
      {
        cond = cmp != 0;
        break;
      }

      case BC_INST_OP_REL_LESS:
      {
        cond = cmp < 0;
        break;
      }

      case BC_INST_OP_REL_GREATER:
      {
        cond = cmp > 0;
        break;
      }

      default:
      {
        // This is here to silence a compiler warning in release mode.
        cond = 0;
        assert(false);
        break;
      }
    }
  }

  init = cond ? bc_num_one : bc_num_zero;
  init(&result.data.num);

  status = bc_program_binaryOpRetire(p, &result, BC_RESULT_INTERMEDIATE);
  if (status) goto err;

  return status;

err:

  bc_num_free(&result.data.num);

  return status;
}

BcNumBinaryFunc bc_program_assignOp(uint8_t inst) {

  switch (inst) {

    case BC_INST_OP_ASSIGN_POWER:
    {
      return bc_num_pow;
    }

    case BC_INST_OP_ASSIGN_MULTIPLY:
    {
      return bc_num_mul;
    }

    case BC_INST_OP_ASSIGN_DIVIDE:
    {
      return bc_num_div;
    }

    case BC_INST_OP_ASSIGN_MODULUS:
    {
      return bc_num_mod;
    }

    case BC_INST_OP_ASSIGN_PLUS:
    {
      return bc_num_add;
    }

    case BC_INST_OP_ASSIGN_MINUS:
    {
      return bc_num_sub;
    }

    default:
    {
      assert(false);
      return NULL;
    }
  }
}

BcStatus bc_program_assignScale(BcProgram *p, BcNum *scale,
                                BcNum *rval, uint8_t inst)
{
  BcStatus status;
  unsigned long result;

  switch (inst) {

    case BC_INST_OP_ASSIGN_POWER:
    case BC_INST_OP_ASSIGN_MULTIPLY:
    case BC_INST_OP_ASSIGN_DIVIDE:
    case BC_INST_OP_ASSIGN_MODULUS:
    case BC_INST_OP_ASSIGN_PLUS:
    case BC_INST_OP_ASSIGN_MINUS:
    {
      BcNumBinaryFunc op = bc_program_assignOp(inst);
      status = op(scale, rval, scale, p->scale);
      break;
    }

    case BC_INST_OP_ASSIGN:
    {
      status = bc_num_copy(scale, rval);
      break;
    }

    default:
    {
      // This is here to silence a compiler warning in release mode.
      status = BC_STATUS_SUCCESS;
      assert(false);
      break;
    }
  }

  if (status) return status;

  if ((status = bc_num_ulong(scale, &result))) return status;

  if (result > (unsigned long) p->scale_max) return BC_STATUS_EXEC_BAD_SCALE;

  p->scale = (size_t) result;

  return status;
}

BcStatus bc_program_assign(BcProgram *p, uint8_t inst) {

  BcStatus status;
  BcResult *left, *right, result;
  BcNum *lval, *rval;

  status = bc_program_binaryOpPrep(p, &left, &lval, &right, &rval);
  if (status) return status;

  if (left->type == BC_RESULT_CONSTANT || left->type == BC_RESULT_INTERMEDIATE)
    return BC_STATUS_PARSE_BAD_ASSIGN;

  if (inst == BC_EXPR_ASSIGN_DIVIDE && !bc_num_cmp(rval, &p->zero, NULL))
    return BC_STATUS_MATH_DIVIDE_BY_ZERO;

  if (left->type != BC_RESULT_SCALE) {

    switch (inst) {

      case BC_INST_OP_ASSIGN_POWER:
      case BC_INST_OP_ASSIGN_MULTIPLY:
      case BC_INST_OP_ASSIGN_DIVIDE:
      case BC_INST_OP_ASSIGN_MODULUS:
      case BC_INST_OP_ASSIGN_PLUS:
      case BC_INST_OP_ASSIGN_MINUS:
      {
        BcNumBinaryFunc op = bc_program_assignOp(inst);
        status = op(lval, rval, lval, p->scale);
        break;
      }

      case BC_INST_OP_ASSIGN:
      {
        status = bc_num_copy(lval, rval);
        break;
      }

      default:
      {
        assert(false);
        break;
      }
    }

    if (status) return status;

    if (left->type == BC_RESULT_IBASE || left->type == BC_RESULT_OBASE) {

      unsigned long base, max;
      size_t *ptr;

      ptr = left->type == BC_RESULT_IBASE ? &p->ibase_t : &p->obase_t;
      max = left->type == BC_RESULT_IBASE ? BC_NUM_MAX_INPUT_BASE : p->base_max;

      if ((status = bc_num_ulong(lval, &base))) return status;

      if (base < BC_NUM_MIN_BASE || base > max)
        return left->type - BC_RESULT_IBASE + BC_STATUS_EXEC_BAD_IBASE;

      *ptr = (size_t) base;
    }
  }
  else if ((status = bc_program_assignScale(p, lval, rval, inst)))
    return status;

  if ((status = bc_num_init(&result.data.num, lval->len))) return status;
  if ((status = bc_num_copy(&result.data.num, lval))) goto err;

  status = bc_program_binaryOpRetire(p, &result, BC_RESULT_INTERMEDIATE);
  if (status) goto err;

  return status;

err:

  bc_num_free(&result.data.num);

  return status;
}

BcStatus bc_program_call(BcProgram *p, uint8_t *code, size_t *idx) {

  BcStatus status;
  BcInstPtr ip;
  size_t nparams, i;
  BcFunc *func;
  BcAuto *auto_ptr;
  BcResult param, *arg;

  status = BC_STATUS_SUCCESS;
  nparams = bc_program_index(code, idx);

  ip.idx = 0;
  ip.len = p->results.len;
  ip.func = bc_program_index(code, idx);

  func = bc_vec_item(&p->funcs, ip.func);

  assert(func);

  if (!func->code.len) return BC_STATUS_EXEC_UNDEFINED_FUNC;
  if (nparams != func->nparams) return BC_STATUS_EXEC_MISMATCHED_PARAMS;

  for (i = 0; i < nparams; ++i) {

    auto_ptr = bc_vec_item(&func->autos, i);
    arg = bc_vec_item_rev(&p->results, nparams - 1);
    assert(auto_ptr && arg);
    param.type = auto_ptr->var + BC_RESULT_ARRAY_AUTO;

    if (auto_ptr->var) {

      BcNum *n;

      if ((status = bc_program_num(p, arg, &n, false))) return status;
      if ((status = bc_num_init(&param.data.num, n->len))) return status;

      status = bc_num_copy(&param.data.num, n);
    }
    else {

      BcVec *a;

      if (arg->type != BC_RESULT_VAR || arg->type != BC_RESULT_ARRAY)
        return BC_STATUS_EXEC_BAD_TYPE;

      status = bc_program_search(p, arg, (BcNum**) &a, BC_PROGRAM_SEARCH_ARRAY);
      if (status) return status;

      status = bc_vec_init(&param.data.array, sizeof(BcNum), bc_num_free);
      if (status) return status;

      status = bc_array_copy(&param.data.array, a);
    }

    if (status) goto err;

    status = bc_vec_push(&p->results, &param);
  }

  for (; i < func->autos.len; ++i) {

    auto_ptr = bc_vec_item_rev(&func->autos, i);
    assert(auto_ptr);
    param.type = auto_ptr->var + BC_RESULT_ARRAY_AUTO;

    if (auto_ptr->var) status = bc_num_init(&param.data.num, BC_NUM_DEF_SIZE);
    else status = bc_vec_init(&param.data.array, sizeof(BcNum), bc_num_free);

    if (status) return status;

    status = bc_vec_push(&p->results, &param);
  }

  if (status) goto err;

  return bc_vec_push(&p->stack, &ip);

err:

  bc_result_free(&param);

  return status;
}

BcStatus bc_program_return(BcProgram *p, uint8_t inst) {

  BcStatus status;
  BcResult result, *operand;
  BcInstPtr *ip;
  BcFunc *func;

  assert(BC_PROGRAM_CHECK_STACK(p));

  ip = bc_vec_top(&p->stack);
  assert(ip);
  assert(BC_PROGRAM_CHECK_RESULTS(p, ip->len + inst == BC_INST_RETURN));
  func = bc_vec_item(&p->funcs, ip->func);
  assert(func);

  result.type = BC_RESULT_INTERMEDIATE;

  if (inst == BC_INST_RETURN) {

    BcNum *num;

    operand = bc_vec_top(&p->results);

    assert(operand);

    if ((status = bc_program_num(p, operand, &num, false))) return status;
    if ((status = bc_num_init(&result.data.num, num->len))) return status;
    if ((status = bc_num_copy(&result.data.num, num))) goto err;
  }
  else {
    status = bc_num_init(&result.data.num, BC_NUM_DEF_SIZE);
    if (status) return status;
    bc_num_zero(&result.data.num);
  }

  // We need to pop arguments as well, so this takes that into account.
  status = bc_vec_npop(&p->results, p->results.len - (ip->len - func->nparams));
  if (status) goto err;

  if ((status = bc_vec_push(&p->results, &result))) goto err;

  return bc_vec_pop(&p->stack);

err:

  bc_num_free(&result.data.num);

  return status;
}

unsigned long bc_program_scale(BcNum *n) {
  return (unsigned long) n->rdx;
}

unsigned long bc_program_length(BcNum *n) {

  unsigned long len = n->len;

  if (n->rdx == n->len) {
    size_t i;
    for (i = n->len - 1; i < n->len && !n->num[i]; --len, --i);
  }

  return len;
}

BcStatus bc_program_builtin(BcProgram *p, uint8_t inst) {

  BcStatus status;
  BcResult *operand;
  BcNum *num1;
  BcResult result;

  if ((status = bc_program_unaryOpPrep(p, &operand, &num1))) return status;
  if ((status = bc_num_init(&result.data.num, BC_NUM_DEF_SIZE))) return status;

  if (inst == BC_INST_SQRT) {
    status = bc_num_sqrt(num1, &result.data.num, p->scale);
  }
  else {

    BcProgramBuiltInFunc func;
    unsigned long ans;

    func = inst == BC_INST_LENGTH ? bc_program_length : bc_program_scale;
    ans = func(num1);

    status = bc_num_ulong2num(&result.data.num, ans);
  }

  if (status) goto err;

  status = bc_program_unaryOpRetire(p, &result, BC_RESULT_INTERMEDIATE);
  if (status) goto err;

  return status;

err:

  bc_num_free(&result.data.num);

  return status;
}

BcStatus bc_program_pushScale(BcProgram *p) {

  BcStatus status;
  BcResult result;

  result.type = BC_RESULT_SCALE;

  if ((status = bc_num_init(&result.data.num, BC_NUM_DEF_SIZE))) return status;

  status = bc_num_ulong2num(&result.data.num, (unsigned long) p->scale);
  if (status) goto err;

  if ((status = bc_vec_push(&p->results, &result))) goto err;

  return status;

err:

  bc_num_free(&result.data.num);

  return status;
}

BcStatus bc_program_incdec(BcProgram *p, uint8_t inst) {

  BcStatus status;
  BcResult *ptr, result, copy;
  BcNum *num;
  uint8_t inst2;

  if ((status = bc_program_unaryOpPrep(p, &ptr, &num))) return status;

  inst2 = inst == BC_INST_INC || inst == BC_INST_INC_DUP ?
            BC_INST_OP_ASSIGN_PLUS : BC_INST_OP_ASSIGN_MINUS;

  if (inst == BC_INST_INC_DUP || inst == BC_INST_DEC_DUP) {
    copy.type = BC_RESULT_INTERMEDIATE;
    if ((status = bc_num_init(&copy.data.num, num->len))) return status;
  }

  result.type = BC_RESULT_ONE;

  if ((status = bc_vec_push(&p->results, &result))) goto err;
  if ((status = bc_program_assign(p, inst2))) goto err;

  if (inst == BC_INST_INC_DUP || inst == BC_INST_DEC_DUP) {
    if ((status = bc_vec_pop(&p->results))) goto err;
    if ((status = bc_vec_push(&p->results, &copy))) goto err;
  }

  return status;

err:

  if (inst == BC_INST_INC_DUP || inst == BC_INST_DEC_DUP)
    bc_num_free(&copy.data.num);

  return status;
}

BcStatus bc_program_init(BcProgram *p) {

  BcStatus s;
  size_t idx;
  char *main_name, *read_name;
  BcInstPtr ip;

  assert(p);

  main_name = read_name = NULL;
  p->nchars = 0;

#ifdef _POSIX_BC_BASE_MAX
  p->base_max = _POSIX_BC_BASE_MAX;
#elif defined(_BC_BASE_MAX)
  p->base_max = _BC_BASE_MAX;
#else
  p->base_max = sysconf(_SC_BC_BASE_MAX);
#endif

  assert(p->base_max <= BC_BASE_MAX_DEF);
  p->base_max = BC_BASE_MAX_DEF;

#ifdef _POSIX_BC_DIM_MAX
  p->dim_max = _POSIX_BC_DIM_MAX;
#elif defined(_BC_DIM_MAX)
  p->dim_max = _BC_DIM_MAX;
#else
  p->dim_max = sysconf(_SC_BC_DIM_MAX);
#endif

  assert(p->dim_max <= BC_DIM_MAX_DEF);
  p->dim_max = BC_DIM_MAX_DEF;

#ifdef _POSIX_BC_SCALE_MAX
  p->scale_max = _POSIX_BC_SCALE_MAX;
#elif defined(_BC_SCALE_MAX)
  p->scale_max = _BC_SCALE_MAX;
#else
  p->scale_max = sysconf(_SC_BC_SCALE_MAX);
#endif

  assert(p->scale_max <= BC_SCALE_MAX_DEF);
  p->scale_max = BC_SCALE_MAX_DEF;

#ifdef _POSIX_BC_STRING_MAX
  p->string_max = _POSIX_BC_STRING_MAX;
#elif defined(_BC_STRING_MAX)
  p->string_max = _BC_STRING_MAX;
#else
  p->string_max = sysconf(_SC_BC_STRING_MAX);
#endif

  assert(p->string_max <= BC_STRING_MAX_DEF);
  p->string_max = BC_STRING_MAX_DEF;

  p->scale = 0;

  if ((s = bc_num_init(&p->ibase, BC_NUM_DEF_SIZE))) return s;
  bc_num_ten(&p->ibase);
  p->ibase_t = 10;

  if ((s = bc_num_init(&p->obase, BC_NUM_DEF_SIZE))) goto obase_err;
  bc_num_ten(&p->obase);
  p->obase_t = 10;

  if ((s = bc_num_init(&p->last, BC_NUM_DEF_SIZE))) goto last_err;
  bc_num_zero(&p->last);

  if ((s = bc_num_init(&p->zero, BC_NUM_DEF_SIZE))) goto zero_err;
  bc_num_zero(&p->zero);

  if ((s = bc_num_init(&p->one, BC_NUM_DEF_SIZE))) goto one_err;
  bc_num_one(&p->one);

  if ((s = bc_vec_init(&p->funcs, sizeof(BcFunc), bc_func_free))) goto func_err;

  s = bc_veco_init(&p->func_map, sizeof(BcEntry), bc_entry_free, bc_entry_cmp);
  if (s) goto func_map_err;

  if (!(main_name = malloc(strlen(bc_lang_func_main) + 1))) {
    s = BC_STATUS_MALLOC_FAIL;
    goto name_err;
  }

  strcpy(main_name, bc_lang_func_main);
  s = bc_program_addFunc(p, main_name, &idx);
  main_name = NULL;
  if (s || idx != BC_PROGRAM_MAIN) goto read_err;

  if (!(read_name = malloc(strlen(bc_lang_func_read) + 1))) {
    s = BC_STATUS_MALLOC_FAIL;
    goto read_err;
  }

  strcpy(read_name, bc_lang_func_read);
  s = bc_program_addFunc(p, read_name, &idx);
  read_name = NULL;
  if (s || idx != BC_PROGRAM_READ) goto var_err;

  ;

  if ((s = bc_vec_init(&p->vars, sizeof(BcNum), bc_num_free))) goto var_err;

  s = bc_veco_init(&p->var_map, sizeof(BcEntry), bc_entry_free, bc_entry_cmp);
  if (s) goto var_map_err;

  if ((s = bc_vec_init(&p->arrays, sizeof(BcVec), bc_vec_free))) goto array_err;

  s = bc_veco_init(&p->array_map, sizeof(BcEntry), bc_entry_free, bc_entry_cmp);
  if (s) goto array_map_err;

  s = bc_vec_init(&p->strings, sizeof(char*), bc_string_free);
  if (s) goto string_err;

  s = bc_vec_init(&p->constants, sizeof(char*), bc_string_free);
  if (s) goto const_err;

  s = bc_vec_init(&p->results, sizeof(BcResult), bc_result_free);
  if (s) goto expr_err;

  if ((s = bc_vec_init(&p->stack, sizeof(BcInstPtr), NULL))) goto stack_err;

  ip.idx = 0;
  ip.func = 0;
  ip.len = 0;

  if ((s = bc_vec_push(&p->stack, &ip))) goto push_err;

  return s;

push_err:

  bc_vec_free(&p->stack);

stack_err:

  bc_vec_free(&p->results);

expr_err:

  bc_vec_free(&p->constants);

const_err:

  bc_vec_free(&p->strings);

string_err:

  bc_veco_free(&p->array_map);

array_map_err:

  bc_vec_free(&p->arrays);

array_err:

  bc_veco_free(&p->var_map);

var_map_err:

  bc_vec_free(&p->vars);

var_err:

  if (read_name) free(read_name);

read_err:

  if (main_name) free(main_name);

name_err:

  bc_veco_free(&p->func_map);

func_map_err:

  bc_vec_free(&p->funcs);

func_err:

  bc_num_free(&p->one);

one_err:

  bc_num_free(&p->zero);

zero_err:

  bc_num_free(&p->last);

last_err:

  bc_num_free(&p->obase);

obase_err:

  bc_num_free(&p->ibase);

  return s;
}

BcStatus bc_program_addFunc(BcProgram *p, char *name, size_t *idx) {

  BcStatus status;
  BcEntry entry, *entry_ptr;
  BcFunc f;

  assert(p && name && idx);

  entry.name = name;
  entry.idx = p->funcs.len;

  if ((status = bc_veco_insert(&p->func_map, &entry, idx))) {
    free(name);
    if (status != BC_STATUS_VEC_ITEM_EXISTS) return status;
  }

  entry_ptr = bc_veco_item(&p->func_map, *idx);
  assert(entry_ptr);
  *idx = entry_ptr->idx;

  if (status == BC_STATUS_VEC_ITEM_EXISTS) {

    BcFunc *func = bc_vec_item(&p->funcs, entry_ptr->idx);

    assert(func);

    status = BC_STATUS_SUCCESS;

    // We need to reset these, so the function can be repopulated.
    func->nparams = 0;
    if ((status = bc_vec_npop(&func->autos, func->autos.len))) return status;
    if ((status = bc_vec_npop(&func->code, func->code.len))) return status;
    status = bc_vec_npop(&func->labels, func->labels.len);
  }
  else {
    if ((status = bc_func_init(&f))) return status;
    status = bc_vec_push(&p->funcs, &f);
    if (status) bc_func_free(&f);
  }

  return status;
}

BcStatus bc_program_exec(BcProgram *p) {

  BcStatus status;
  uint8_t *code;
  size_t idx;
  BcResult result;
  BcFunc *func;
  BcInstPtr *ip;
  bool cond;

  status = BC_STATUS_SUCCESS;
  cond = false;

  ip = bc_vec_top(&p->stack);
  assert(ip);
  func = bc_vec_item(&p->funcs, ip->func);
  assert(func);
  code = func->code.array;

  while (!bcg.sig_int && ip->idx < func->code.len) {

    uint8_t inst = code[(ip->idx)++];

    switch (inst) {

      case BC_INST_CALL:
      {
        status = bc_program_call(p, code, &ip->idx);
        break;
      }

      case BC_INST_RETURN:
      case BC_INST_RETURN_ZERO:
      {
        status = bc_program_return(p, inst);
        break;
      }

      case BC_INST_READ:
      {
        status = bc_program_read(p);
        break;
      }

      case BC_INST_JUMP_ZERO:
      {
        BcResult *operand;
        BcNum *num;

        if ((status = bc_program_unaryOpPrep(p, &operand, &num))) return status;
        cond = bc_num_cmp(num, &p->zero, NULL) == 0;
        status = bc_vec_pop(&p->results);
      }
      // Fallthrough.
      case BC_INST_JUMP:
      {
        size_t idx;
        size_t *addr;

        idx = bc_program_index(code, &ip->idx);
        addr = bc_vec_item(&func->labels, idx);

        assert(addr);

        if (inst == BC_INST_JUMP || cond) ip->idx = *addr;

        break;
      }

      case BC_INST_PUSH_VAR:
      case BC_INST_PUSH_ARRAY:
      {
        status = bc_program_push(p, code, &ip->idx, inst == BC_INST_PUSH_VAR);
        break;
      }

      case BC_INST_PUSH_LAST:
      {
        result.type = BC_RESULT_LAST;
        status = bc_vec_push(&p->results, &result);
        break;
      }

      case BC_INST_PUSH_SCALE:
      {
        status = bc_program_pushScale(p);
        break;
      }

      case BC_INST_PUSH_IBASE:
      {
        result.type = BC_RESULT_IBASE;
        status = bc_vec_push(&p->results, &result);
        break;
      }

      case BC_INST_PUSH_OBASE:
      {
        result.type = BC_RESULT_OBASE;
        status = bc_vec_push(&p->results, &result);
        break;
      }

      case BC_INST_SCALE_FUNC:
      case BC_INST_LENGTH:
      case BC_INST_SQRT:
      {
        status = bc_program_builtin(p, inst);
        break;
      }

      case BC_INST_PUSH_NUM:
      {
        result.type = BC_RESULT_CONSTANT;
        result.data.id.idx = bc_program_index(code, &ip->idx);
        status = bc_vec_push(&p->results, &result);
        break;
      }

      case BC_INST_POP:
      {
        status = bc_vec_pop(&p->results);
        break;
      }

      case BC_INST_INC_DUP:
      case BC_INST_DEC_DUP:
      case BC_INST_INC:
      case BC_INST_DEC:
      {
        status = bc_program_incdec(p, inst);
        break;
      }

      case BC_INST_HALT:
      {
        status = BC_STATUS_QUIT;
        break;
      }

      case BC_INST_PRINT:
      case BC_INST_PRINT_EXPR:
      {
        BcResult *operand;
        BcNum *num;
        bool newline;

        if ((status = bc_program_unaryOpPrep(p, &operand, &num))) return status;

        newline = inst == BC_INST_PRINT;
        status = bc_num_print(num, &p->obase, p->obase_t, newline, &p->nchars);
        if (status) return status;

        if ((status = bc_num_copy(&p->last, num))) return status;

        status = bc_vec_pop(&p->results);

        break;
      }

      case BC_INST_STR:
      {
        const char **string, *s;
        size_t len;

        idx = bc_program_index(code, &ip->idx);
        assert(idx < p->strings.len);
        string = bc_vec_item(&p->strings, idx);
        assert(string);

        s = *string;
        len = strlen(s);

        for (idx = 0; idx < len; ++idx) {
          char c = s[idx];
          if (putchar(c) == EOF) return BC_STATUS_IO_ERR;
          if (c == '\n') p->nchars = SIZE_MAX;
          ++p->nchars;
        }

        break;
      }

      case BC_INST_PRINT_STR:
      {
        const char **string;

        idx = bc_program_index(code, &ip->idx);
        assert(idx < p->strings.len);
        string = bc_vec_item(&p->strings, idx);
        assert(string);

        status = bc_program_printString(*string, &p->nchars);

        break;
      }

      case BC_INST_OP_POWER:
      case BC_INST_OP_MULTIPLY:
      case BC_INST_OP_DIVIDE:
      case BC_INST_OP_MODULUS:
      case BC_INST_OP_PLUS:
      case BC_INST_OP_MINUS:
      {
        status = bc_program_op(p, inst);
        break;
      }

      case BC_INST_OP_REL_EQUAL:
      case BC_INST_OP_REL_LESS_EQ:
      case BC_INST_OP_REL_GREATER_EQ:
      case BC_INST_OP_REL_NOT_EQ:
      case BC_INST_OP_REL_LESS:
      case BC_INST_OP_REL_GREATER:
      {
        status = bc_program_logical(p, inst);
        break;
      }

      case BC_INST_OP_BOOL_NOT:
      {
        BcResult *ptr;
        BcNum *num;

        if ((status = bc_program_unaryOpPrep(p, &ptr, &num))) return status;

        status = bc_num_init(&result.data.num, BC_NUM_DEF_SIZE);
        if (status) return status;

        if (bc_num_cmp(num, &p->zero, NULL)) bc_num_one(&result.data.num);
        else bc_num_zero(&result.data.num);

        status = bc_program_unaryOpRetire(p, &result, BC_RESULT_INTERMEDIATE);

        if (status) bc_num_free(&result.data.num);

        break;
      }

      case BC_INST_OP_BOOL_OR:
      case BC_INST_OP_BOOL_AND:
      {
        status = bc_program_logical(p, inst);
        break;
      }

      case BC_INST_OP_NEGATE:
      {
        status = bc_program_negate(p);
        break;
      }

      case BC_INST_OP_ASSIGN_POWER:
      case BC_INST_OP_ASSIGN_MULTIPLY:
      case BC_INST_OP_ASSIGN_DIVIDE:
      case BC_INST_OP_ASSIGN_MODULUS:
      case BC_INST_OP_ASSIGN_PLUS:
      case BC_INST_OP_ASSIGN_MINUS:
      case BC_INST_OP_ASSIGN:
      {
        status = bc_program_assign(p, inst);
        break;
      }

      default:
      {
        assert(false);
        break;
      }
    }

    if (status) return status;

    // We keep getting these because if the size of the
    // stack changes, pointers may end up being invalid.
    ip = bc_vec_top(&p->stack);
    assert(ip);
    func = bc_vec_item(&p->funcs, ip->func);
    assert(func);
    code = func->code.array;
  }

  return status;
}

BcStatus bc_program_print(BcProgram *p) {

  BcStatus status;
  BcFunc *func;
  uint8_t *code;
  BcInstPtr ip;
  size_t i;

  status = BC_STATUS_SUCCESS;

  for (i = 0; !status && i < p->funcs.len; ++i) {

    ip.idx = ip.len = 0;
    ip.func = i;

    func = bc_vec_item(&p->funcs, ip.func);
    assert(func);
    code = func->code.array;

    if (printf("func[%zu]: ", ip.func) < 0) return BC_STATUS_IO_ERR;

    while (ip.idx < func->code.len) {

      uint8_t inst = code[ip.idx++];

      switch (inst) {

        case BC_INST_PUSH_VAR:
        case BC_INST_PUSH_ARRAY:
        {
          if (putchar(inst) == EOF) return BC_STATUS_IO_ERR;
          status = bc_program_printName(code, &ip.idx);
          break;
        }

        case BC_INST_CALL:
        {
          if (putchar(inst) == EOF) return BC_STATUS_IO_ERR;
          if ((status = bc_program_printIndex(code, &ip.idx))) return status;
          status = bc_program_printIndex(code, &ip.idx);
          break;
        }

        case BC_INST_JUMP:
        case BC_INST_JUMP_ZERO:
        case BC_INST_PUSH_NUM:
        case BC_INST_STR:
        case BC_INST_PRINT_STR:
        {
          if (putchar(inst) == EOF) return BC_STATUS_IO_ERR;
          bc_program_printIndex(code, &ip.idx);
          break;
        }

        default:
        {
          if (putchar(inst) == EOF) return BC_STATUS_IO_ERR;
          break;
        }
      }
    }

    if (status) return status;

    if (putchar('\n') == EOF) status = BC_STATUS_IO_ERR;
  }

  return status;
}

void bc_program_free(BcProgram *p) {

  if (!p) return;

  bc_num_free(&p->ibase);
  bc_num_free(&p->obase);

  bc_vec_free(&p->funcs);
  bc_veco_free(&p->func_map);

  bc_vec_free(&p->vars);
  bc_veco_free(&p->var_map);

  bc_vec_free(&p->arrays);
  bc_veco_free(&p->array_map);

  bc_vec_free(&p->strings);
  bc_vec_free(&p->constants);

  bc_vec_free(&p->results);
  bc_vec_free(&p->stack);

  bc_num_free(&p->last);
  bc_num_free(&p->zero);
  bc_num_free(&p->one);

  memset(p, 0, sizeof(BcProgram));
}
