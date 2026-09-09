/* Core-facing portion of CrankBoyHQ/pdll, commit 2e18c1d9275e3bbe11ab7b76201b9cac90748a77.
 * pdll is MIT licensed. The frontend owns the loader; an emucore only needs
 * the handshake structure and export-table macros below. */
#ifndef PDLL_H
#define PDLL_H

#include "pd_api.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PDLL_DYNAMIC_INIT_ARG 0x17403900u

typedef void *(*pdll_getsymbol_t)(const char *symbol);
typedef int (*pdll_eventhandler_t)(PlaydateAPI *, PDSystemEvent, uint32_t);

typedef struct pdll_s {
  struct {
    const struct playdate_sys *system;
    const struct playdate_file *file;
    const struct playdate_graphics *graphics;
  } _playdate_slice;
  PlaydateAPI *playdate_ptr;
  pdll_getsymbol_t getSymbol;
  pdll_eventhandler_t eventHandler;
  const char *path;
  uint32_t flags;
  void *image;
  void *image_raw;
#ifdef TARGET_SIMULATOR
  void *dl;
  char tmppath[1024];
#endif
} pdll_t;

typedef struct {
  const char *name;
  void *addr;
} pdll_export_t;

#define PDLL__EVAL0(...) __VA_ARGS__
#define PDLL__EVAL1(...) PDLL__EVAL0(PDLL__EVAL0(PDLL__EVAL0(__VA_ARGS__)))
#define PDLL__EVAL2(...) PDLL__EVAL1(PDLL__EVAL1(PDLL__EVAL1(__VA_ARGS__)))
#define PDLL__EVAL(...) PDLL__EVAL2(PDLL__EVAL2(PDLL__EVAL2(__VA_ARGS__)))
#define PDLL__MAP_END(...)
#define PDLL__MAP_OUT
#define PDLL__MAP_GET_END2() 0, PDLL__MAP_END
#define PDLL__MAP_GET_END1(...) PDLL__MAP_GET_END2
#define PDLL__MAP_GET_END(...) PDLL__MAP_GET_END1
#define PDLL__MAP_NEXT0(test, next, ...) next PDLL__MAP_OUT
#define PDLL__MAP_NEXT1(test, next) PDLL__MAP_NEXT0(test, next, 0)
#define PDLL__MAP_NEXT(test, next) PDLL__MAP_NEXT1(PDLL__MAP_GET_END test, next)
#define PDLL__MAP0(f, x, peek, ...) \
  f(x) PDLL__MAP_NEXT(peek, PDLL__MAP1)(f, peek, __VA_ARGS__)
#define PDLL__MAP1(f, x, peek, ...) \
  f(x) PDLL__MAP_NEXT(peek, PDLL__MAP0)(f, peek, __VA_ARGS__)
#define PDLL__MAP(f, ...) \
  PDLL__EVAL(PDLL__MAP1(f, __VA_ARGS__, ()()(), ()()(), ()()(), 0))

#define PDLL__ARG3(a, b, c, ...) c
#define PDLL__HAS_COMMA(...) PDLL__ARG3(__VA_ARGS__, 1, 0)
#define PDLL__TRIGGER(...) ,
#define PDLL__PASTE5(a, b, c, d, e) a##b##c##d##e
#define PDLL__IS_EMPTY_CASE_0001 ,
#define PDLL__ISEMPTY_(a, b, c, d) \
  PDLL__HAS_COMMA(PDLL__PASTE5(PDLL__IS_EMPTY_CASE_, a, b, c, d))
#define PDLL__ISEMPTY(...) \
  PDLL__ISEMPTY_(PDLL__HAS_COMMA(__VA_ARGS__), \
                 PDLL__HAS_COMMA(PDLL__TRIGGER __VA_ARGS__), \
                 PDLL__HAS_COMMA(__VA_ARGS__()), \
                 PDLL__HAS_COMMA(PDLL__TRIGGER __VA_ARGS__()))

#define PDLL__STR(x) #x
#define PDLL__EXPORT_ENTRY(fn) {PDLL__STR(fn), (void *)(fn)},
#define PDLL__EXPORTS_1(...)
#define PDLL__EXPORTS_0(...) PDLL__MAP(PDLL__EXPORT_ENTRY, __VA_ARGS__)
#define PDLL__CAT(a, b) a##b
#define PDLL__XCAT(a, b) PDLL__CAT(a, b)

#define PDLL_EXPORT(...) \
  static const pdll_export_t pdll__exports[] = { \
      PDLL__XCAT(PDLL__EXPORTS_, PDLL__ISEMPTY(__VA_ARGS__))(__VA_ARGS__){0, 0}}; \
  static void *pdll_getsymbol_impl(const char *symbol) { \
    const pdll_export_t *e; \
    for (e = pdll__exports; e->name; ++e) \
      if (!strcmp(symbol, e->name)) return e->addr; \
    return NULL; \
  }

#define PDLL_EVENT(playdate, event, arg) \
  pdll_t *pdll = ((event) == kEventInit && (arg) == PDLL_DYNAMIC_INIT_ARG) \
                     ? (pdll_t *)(void *)(playdate) : (pdll_t *)0; \
  if (pdll) { \
    pdll->getSymbol = pdll_getsymbol_impl; \
    (playdate) = pdll->playdate_ptr; \
  }

#endif /* PDLL_H */
